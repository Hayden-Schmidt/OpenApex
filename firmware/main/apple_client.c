#include "apple_client.h"

#include "board_profile.h"

// The whole GATT-client role compiles out on a board without it, so the call sites in
// ble_link.c and main.c stay unconditional (same pattern as touch_driver.c).
#if BOARD_HAS_ANCS

#include "cts_client.h"

#include "esp_log.h"

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"

#include <string.h>

static const char *TAG = "apple";

// ANCS UUIDs, byte-reversed for BLE_UUID128_INIT (which takes wire/little-endian order), from the
// canonical strings in ancs_core.h.
// Service 7905F431-B5CE-4E99-A40F-4B1E122D00D0
static const ble_uuid128_t s_ancs_svc_uuid =
    BLE_UUID128_INIT(0xd0, 0x00, 0x2d, 0x12, 0x1e, 0x4b, 0x0f, 0xa4, 0x99, 0x4e, 0xce, 0xb5, 0x31,
                     0xf4, 0x05, 0x79);
// Notification Source 9FBF120D-6301-42D9-8C58-25E699A21DBD
static const ble_uuid128_t s_ancs_ns_uuid =
    BLE_UUID128_INIT(0xbd, 0x1d, 0xa2, 0x99, 0xe6, 0x25, 0x58, 0x8c, 0xd9, 0x42, 0x01, 0x63, 0x0d,
                     0x12, 0xbf, 0x9f);
// Control Point 69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9
static const ble_uuid128_t s_ancs_cp_uuid =
    BLE_UUID128_INIT(0xd9, 0xd9, 0xaa, 0xfd, 0xbd, 0x9b, 0x21, 0x98, 0xa8, 0x49, 0xe1, 0x45, 0xf3,
                     0xd8, 0xd1, 0x69);
// Data Source 22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB
static const ble_uuid128_t s_ancs_ds_uuid =
    BLE_UUID128_INIT(0xfb, 0x7b, 0x7c, 0xce, 0x6a, 0xb3, 0x44, 0xbe, 0xb5, 0x4b, 0xd6, 0x24, 0xe9,
                     0xc6, 0xea, 0x22);

static const ble_uuid16_t s_cts_svc_uuid = BLE_UUID16_INIT(CTS_SERVICE_UUID16);
static const ble_uuid16_t s_cts_chr_uuid = BLE_UUID16_INIT(CTS_CHR_CURRENT_TIME_UUID16);
static const ble_uuid16_t s_gatt_svc_uuid = BLE_UUID16_INIT(0x1801);
static const ble_uuid16_t s_service_changed_uuid = BLE_UUID16_INIT(0x2A05);
static const ble_uuid16_t s_cccd_uuid = BLE_UUID16_INIT(0x2902);

#define CCCD_NOTIFY 0x0001u
#define CCCD_INDICATE 0x0002u

// iOS publishes and unpublishes ANCS, and a phone that has just connected may not have it up yet.
// Retry rather than treat the first empty discovery as the answer.
#define DISCOVERY_RETRY_MS 5000u
#define DISCOVERY_MAX_TRIES 12u

typedef struct {
    uint16_t svc_start, svc_end;
    uint16_t ns_val, cp_val, ds_val;
    uint16_t ns_cccd, ds_cccd;
    bool ns_subscribed, ds_subscribed;
} ancs_handles_t;

typedef struct {
    uint16_t svc_start, svc_end;
    uint16_t chr_val;
    uint16_t cccd;
} cts_handles_t;

static apple_client_cfg_t s_cfg;
static ancs_core_t s_ancs;
static ancs_handles_t s_h;
static cts_handles_t s_cts;
static uint16_t s_service_changed_handle;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool s_encrypted;
static bool s_discovering;
static uint32_t s_retry_at_ms;
static unsigned s_tries;

static void start_discovery(void);

// -------------------------------------------------------------------------------------------
// ancs_core plumbing
// -------------------------------------------------------------------------------------------

static void write_control_point(const uint8_t *data, size_t len, void *ctx) {
    (void)ctx;
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_h.cp_val == 0) {
        return;
    }
    // Write WITH response: the Control Point answers ATT errors that matter (an unknown UID, a
    // command the phone refuses), and losing one silently would strand the fetch state machine.
    const int rc = ble_gattc_write_flat(s_conn_handle, s_h.cp_val, data, (uint16_t)len, NULL, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "control point write failed; rc=%d", rc);
    }
}

static void on_ancs_notification(const ancs_notification_t *n, bool removed, void *ctx) {
    (void)ctx;
    if (s_cfg.on_notification != NULL) {
        s_cfg.on_notification(n, removed);
    }
}

// -------------------------------------------------------------------------------------------
// Discovery. ANCS first (it is the reason we are a client at all), then CTS, then the GATT
// Service Changed indication so a later publish/unpublish is noticed.
// -------------------------------------------------------------------------------------------

static int write_cccd(uint16_t handle, uint16_t value, ble_gatt_attr_fn *cb) {
    const uint8_t v[2] = {(uint8_t)(value & 0xFFu), (uint8_t)(value >> 8)};
    return ble_gattc_write_flat(s_conn_handle, handle, v, sizeof(v), cb, NULL);
}

static void discover_cts(void);
static void discover_service_changed(void);

// The two subscribes run in sequence, one outstanding GATT procedure at a time, and Data Source
// goes FIRST: subscribing to the Notification Source is what makes iOS replay the notifications
// already sitting on the phone, and those immediately provoke attribute fetches whose responses
// would land on a Data Source nobody is listening to.
static int on_ns_subscribed(uint16_t conn, const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg) {
    (void)conn;
    (void)attr;
    (void)arg;
    if (error != NULL && error->status != 0) {
        ESP_LOGW(TAG, "notification source subscribe failed; status=%d", error->status);
    } else {
        s_h.ns_subscribed = true;
        ESP_LOGI(TAG, "ANCS ready (ns=%u cp=%u ds=%u)", s_h.ns_val, s_h.cp_val, s_h.ds_val);
    }
    discover_cts();
    return 0;
}

static int on_ds_subscribed(uint16_t conn, const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg) {
    (void)conn;
    (void)attr;
    (void)arg;
    if (error != NULL && error->status != 0) {
        ESP_LOGW(TAG, "data source subscribe failed; status=%d", error->status);
    } else {
        s_h.ds_subscribed = true;
    }
    if (s_h.ns_cccd != 0 && write_cccd(s_h.ns_cccd, CCCD_NOTIFY, on_ns_subscribed) == 0) {
        return 0;
    }
    ESP_LOGW(TAG, "notification source CCCD missing");
    discover_cts();
    return 0;
}

static int on_ancs_dsc(uint16_t conn, const struct ble_gatt_error *error, uint16_t chr_val_handle,
                       const struct ble_gatt_dsc *dsc, void *arg) {
    (void)conn;
    (void)arg;
    if (error != NULL && error->status == BLE_HS_EDONE) {
        // One pass over the whole service range found both CCCDs, attributed to their
        // characteristics by the handle this callback reports alongside each descriptor.
        if (s_h.ds_cccd != 0 && write_cccd(s_h.ds_cccd, CCCD_NOTIFY, on_ds_subscribed) == 0) {
            return 0;
        }
        ESP_LOGW(TAG, "ANCS CCCDs not found (ns=%u ds=%u)", s_h.ns_cccd, s_h.ds_cccd);
        discover_cts();
        return 0;
    }
    if (error != NULL && error->status != 0) {
        ESP_LOGW(TAG, "ANCS descriptor discovery failed; status=%d", error->status);
        discover_cts();
        return 0;
    }
    if (dsc != NULL && ble_uuid_cmp(&dsc->uuid.u, &s_cccd_uuid.u) == 0) {
        if (chr_val_handle == s_h.ns_val) {
            s_h.ns_cccd = dsc->handle;
        } else if (chr_val_handle == s_h.ds_val) {
            s_h.ds_cccd = dsc->handle;
        }
    }
    return 0;
}

static int on_ancs_chr(uint16_t conn, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg) {
    (void)conn;
    (void)arg;
    if (error != NULL && error->status == BLE_HS_EDONE) {
        if (s_h.ns_val == 0 || s_h.cp_val == 0 || s_h.ds_val == 0) {
            ESP_LOGW(TAG, "ANCS incomplete (ns=%u cp=%u ds=%u)", s_h.ns_val, s_h.cp_val, s_h.ds_val);
            discover_cts();
            return 0;
        }
        if (ble_gattc_disc_all_dscs(s_conn_handle, s_h.svc_start, s_h.svc_end, on_ancs_dsc, NULL) !=
            0) {
            discover_cts();
        }
        return 0;
    }
    if (error != NULL && error->status != 0) {
        ESP_LOGW(TAG, "ANCS characteristic discovery failed; status=%d", error->status);
        discover_cts();
        return 0;
    }
    if (chr == NULL) {
        return 0;
    }
    if (ble_uuid_cmp(&chr->uuid.u, &s_ancs_ns_uuid.u) == 0) {
        s_h.ns_val = chr->val_handle;
    } else if (ble_uuid_cmp(&chr->uuid.u, &s_ancs_cp_uuid.u) == 0) {
        s_h.cp_val = chr->val_handle;
    } else if (ble_uuid_cmp(&chr->uuid.u, &s_ancs_ds_uuid.u) == 0) {
        s_h.ds_val = chr->val_handle;
    }
    return 0;
}

static int on_ancs_svc(uint16_t conn, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *svc, void *arg) {
    (void)conn;
    (void)arg;
    if (error != NULL && error->status == BLE_HS_EDONE) {
        if (s_h.svc_start == 0) {
            // No ANCS on this phone right now. Either it is not an iPhone, or iOS has not
            // published the service yet -- which it does on its own schedule, so retry.
            ESP_LOGI(TAG, "no ANCS on this peer (try %u)", s_tries);
            s_discovering = false;
            discover_cts();
            return 0;
        }
        if (ble_gattc_disc_all_chrs(s_conn_handle, s_h.svc_start, s_h.svc_end, on_ancs_chr, NULL) !=
            0) {
            discover_cts();
        }
        return 0;
    }
    if (error != NULL && error->status != 0) {
        ESP_LOGW(TAG, "ANCS service discovery failed; status=%d", error->status);
        s_discovering = false;
        return 0;
    }
    if (svc != NULL) {
        s_h.svc_start = svc->start_handle;
        s_h.svc_end = svc->end_handle;
    }
    return 0;
}

// --- CTS ---

static int on_cts_subscribed(uint16_t conn, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg) {
    (void)conn;
    (void)attr;
    (void)arg;
    if (error != NULL && error->status != 0) {
        ESP_LOGW(TAG, "CTS subscribe failed; status=%d", error->status);
    }
    discover_service_changed();
    return 0;
}

static void deliver_time(const struct os_mbuf *om) {
    uint8_t buf[CTS_CURRENT_TIME_LEN];
    uint16_t copied = 0;
    if (ble_hs_mbuf_to_flat(om, buf, sizeof(buf), &copied) != 0) {
        return;
    }
    uint32_t epoch = 0;
    if (!cts_parse_current_time(buf, copied, &epoch)) {
        // A time we cannot trust leaves the clock showing nothing. Never a fabricated one.
        ESP_LOGW(TAG, "unusable CTS value (%u bytes)", copied);
        return;
    }
    if (s_cfg.on_time != NULL) {
        s_cfg.on_time(epoch);
    }
}

static int on_cts_read(uint16_t conn, const struct ble_gatt_error *error,
                       struct ble_gatt_attr *attr, void *arg) {
    (void)conn;
    (void)arg;
    if (error == NULL || error->status == 0) {
        if (attr != NULL && attr->om != NULL) {
            deliver_time(attr->om);
        }
    } else {
        ESP_LOGW(TAG, "CTS read failed; status=%d", error->status);
    }
    if (s_cts.cccd != 0 && write_cccd(s_cts.cccd, CCCD_NOTIFY, on_cts_subscribed) == 0) {
        return 0;
    }
    discover_service_changed();
    return 0;
}

static int on_cts_dsc(uint16_t conn, const struct ble_gatt_error *error, uint16_t chr_val_handle,
                      const struct ble_gatt_dsc *dsc, void *arg) {
    (void)conn;
    (void)chr_val_handle;
    (void)arg;
    if (error != NULL && error->status == BLE_HS_EDONE) {
        if (ble_gattc_read(s_conn_handle, s_cts.chr_val, on_cts_read, NULL) != 0) {
            discover_service_changed();
        }
        return 0;
    }
    if (error != NULL && error->status != 0) {
        discover_service_changed();
        return 0;
    }
    if (dsc != NULL && ble_uuid_cmp(&dsc->uuid.u, &s_cccd_uuid.u) == 0) {
        s_cts.cccd = dsc->handle;
    }
    return 0;
}

static int on_cts_chr(uint16_t conn, const struct ble_gatt_error *error,
                      const struct ble_gatt_chr *chr, void *arg) {
    (void)conn;
    (void)arg;
    if (error != NULL && error->status == BLE_HS_EDONE) {
        if (s_cts.chr_val == 0) {
            discover_service_changed();
            return 0;
        }
        if (ble_gattc_disc_all_dscs(s_conn_handle, s_cts.chr_val, s_cts.svc_end, on_cts_dsc, NULL) !=
            0) {
            discover_service_changed();
        }
        return 0;
    }
    if (error != NULL && error->status != 0) {
        discover_service_changed();
        return 0;
    }
    if (chr != NULL && ble_uuid_cmp(&chr->uuid.u, &s_cts_chr_uuid.u) == 0) {
        s_cts.chr_val = chr->val_handle;
    }
    return 0;
}

static int on_cts_svc(uint16_t conn, const struct ble_gatt_error *error,
                      const struct ble_gatt_svc *svc, void *arg) {
    (void)conn;
    (void)arg;
    if (error != NULL && error->status == BLE_HS_EDONE) {
        if (s_cts.svc_start == 0) {
            ESP_LOGI(TAG, "no CTS on this peer");
            discover_service_changed();
            return 0;
        }
        if (ble_gattc_disc_all_chrs(s_conn_handle, s_cts.svc_start, s_cts.svc_end, on_cts_chr,
                                    NULL) != 0) {
            discover_service_changed();
        }
        return 0;
    }
    if (error != NULL && error->status != 0) {
        discover_service_changed();
        return 0;
    }
    if (svc != NULL) {
        s_cts.svc_start = svc->start_handle;
        s_cts.svc_end = svc->end_handle;
    }
    return 0;
}

static void discover_cts(void) {
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        s_discovering = false;
        return;
    }
    memset(&s_cts, 0, sizeof(s_cts));
    if (ble_gattc_disc_svc_by_uuid(s_conn_handle, &s_cts_svc_uuid.u, on_cts_svc, NULL) != 0) {
        discover_service_changed();
    }
}

// --- GATT Service Changed ---

static int on_sc_dsc(uint16_t conn, const struct ble_gatt_error *error, uint16_t chr_val_handle,
                     const struct ble_gatt_dsc *dsc, void *arg) {
    (void)conn;
    (void)chr_val_handle;
    (void)arg;
    if (error != NULL && error->status == BLE_HS_EDONE) {
        s_discovering = false;
        return 0;
    }
    if (error != NULL && error->status != 0) {
        s_discovering = false;
        return 0;
    }
    if (dsc != NULL && ble_uuid_cmp(&dsc->uuid.u, &s_cccd_uuid.u) == 0) {
        // Indication, not notification: Service Changed is defined as indicated, and missing one
        // is how a terminal ends up holding handles for a service iOS has since republished.
        (void)write_cccd(dsc->handle, CCCD_INDICATE, NULL);
    }
    return 0;
}

static int on_sc_chr(uint16_t conn, const struct ble_gatt_error *error,
                     const struct ble_gatt_chr *chr, void *arg) {
    (void)conn;
    (void)arg;
    if (error != NULL && error->status == BLE_HS_EDONE) {
        if (s_service_changed_handle == 0) {
            s_discovering = false;
            return 0;
        }
        if (ble_gattc_disc_all_dscs(s_conn_handle, s_service_changed_handle,
                                    s_service_changed_handle + 2, on_sc_dsc, NULL) != 0) {
            s_discovering = false;
        }
        return 0;
    }
    if (error != NULL && error->status != 0) {
        s_discovering = false;
        return 0;
    }
    if (chr != NULL && ble_uuid_cmp(&chr->uuid.u, &s_service_changed_uuid.u) == 0) {
        s_service_changed_handle = chr->val_handle;
    }
    return 0;
}

static int on_sc_svc(uint16_t conn, const struct ble_gatt_error *error,
                     const struct ble_gatt_svc *svc, void *arg) {
    (void)conn;
    (void)arg;
    static uint16_t start, end;
    if (error != NULL && error->status == BLE_HS_EDONE) {
        if (start == 0 ||
            ble_gattc_disc_all_chrs(s_conn_handle, start, end, on_sc_chr, NULL) != 0) {
            s_discovering = false;
        }
        start = 0;
        end = 0;
        return 0;
    }
    if (error != NULL && error->status != 0) {
        s_discovering = false;
        return 0;
    }
    if (svc != NULL) {
        start = svc->start_handle;
        end = svc->end_handle;
    }
    return 0;
}

static void discover_service_changed(void) {
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        s_discovering = false;
        return;
    }
    s_service_changed_handle = 0;
    if (ble_gattc_disc_svc_by_uuid(s_conn_handle, &s_gatt_svc_uuid.u, on_sc_svc, NULL) != 0) {
        s_discovering = false;
    }
}

static void start_discovery(void) {
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_encrypted || s_discovering) {
        return;
    }
    s_discovering = true;
    s_tries++;
    memset(&s_h, 0, sizeof(s_h));
    ancs_core_reset(&s_ancs);
    if (ble_gattc_disc_svc_by_uuid(s_conn_handle, &s_ancs_svc_uuid.u, on_ancs_svc, NULL) != 0) {
        ESP_LOGW(TAG, "could not start ANCS discovery");
        s_discovering = false;
    }
}

// -------------------------------------------------------------------------------------------
// Public surface
// -------------------------------------------------------------------------------------------

void apple_client_init(const apple_client_cfg_t *cfg) {
    s_cfg = *cfg;
    ancs_core_init(&s_ancs, write_control_point, on_ancs_notification, NULL);
    ancs_core_set_raw_tap(&s_ancs, cfg->on_raw);
    memset(&s_h, 0, sizeof(s_h));
    memset(&s_cts, 0, sizeof(s_cts));
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
}

void apple_client_on_connect(uint16_t conn_handle) {
    s_conn_handle = conn_handle;
    s_encrypted = false;
    s_discovering = false;
    s_tries = 0;
    s_retry_at_ms = 0;
    memset(&s_h, 0, sizeof(s_h));
    memset(&s_cts, 0, sizeof(s_cts));
    ancs_core_reset(&s_ancs);
}

void apple_client_on_disconnect(void) {
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_encrypted = false;
    s_discovering = false;
    memset(&s_h, 0, sizeof(s_h));
    memset(&s_cts, 0, sizeof(s_cts));
    // Notification UIDs are per-connection: nothing here may survive into the next link.
    ancs_core_reset(&s_ancs);
}

void apple_client_on_encryption(uint16_t conn_handle, int status) {
    if (status != 0) {
        ESP_LOGW(TAG, "encryption failed; status=%d -- no ANCS without a bond", status);
        return;
    }
    s_conn_handle = conn_handle;
    s_encrypted = true;
    s_tries = 0;
    start_discovery();
}

bool apple_client_on_notify_rx(struct ble_gap_event *event) {
    const uint16_t h = event->notify_rx.attr_handle;
    if (h == 0) {
        return false;
    }
    if (h == s_h.ns_val || h == s_h.ds_val) {
        uint8_t buf[256];
        uint16_t copied = 0;
        if (ble_hs_mbuf_to_flat(event->notify_rx.om, buf, sizeof(buf), &copied) != 0) {
            return true;
        }
        if (h == s_h.ns_val) {
            (void)ancs_core_on_notification_source(&s_ancs, buf, copied);
        } else {
            (void)ancs_core_on_data_source(&s_ancs, buf, copied);
        }
        return true;
    }
    if (h == s_cts.chr_val) {
        deliver_time(event->notify_rx.om);
        return true;
    }
    if (s_service_changed_handle != 0 && h == s_service_changed_handle) {
        // iOS republished its GATT database. Every handle we hold is now suspect, including the
        // ANCS ones, so start over rather than write to a handle that has moved.
        ESP_LOGI(TAG, "service changed; rediscovering");
        s_discovering = false;
        s_tries = 0;
        start_discovery();
        return true;
    }
    return false;
}

void apple_client_tick(uint32_t now_ms) {
    if (!s_encrypted || s_discovering || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    if (s_h.ns_subscribed && s_h.ds_subscribed) {
        return;
    }
    if (s_tries >= DISCOVERY_MAX_TRIES) {
        return;
    }
    if (s_retry_at_ms == 0) {
        s_retry_at_ms = now_ms + DISCOVERY_RETRY_MS;
        return;
    }
    if ((int32_t)(now_ms - s_retry_at_ms) < 0) {
        return;
    }
    s_retry_at_ms = now_ms + DISCOVERY_RETRY_MS;
    start_discovery();
}

bool apple_client_perform_action(uint32_t uid, ancs_action_id_t action) {
    if (s_h.cp_val == 0) {
        return false;
    }
    return ancs_core_perform_action(&s_ancs, uid, action);
}

bool apple_client_ancs_ready(void) {
    return s_h.ns_subscribed && s_h.ds_subscribed;
}

bool apple_client_may_retry(void) {
    return s_discovering || s_tries < DISCOVERY_MAX_TRIES;
}

void apple_client_dump(void) {
    ESP_LOGI(TAG, "conn=%u encrypted=%d discovering=%d tries=%u", s_conn_handle, (int)s_encrypted,
             (int)s_discovering, s_tries);
    ESP_LOGI(TAG, "ANCS svc=%u-%u ns=%u(cccd %u,%s) cp=%u ds=%u(cccd %u,%s)", s_h.svc_start,
             s_h.svc_end, s_h.ns_val, s_h.ns_cccd, s_h.ns_subscribed ? "on" : "off", s_h.cp_val,
             s_h.ds_val, s_h.ds_cccd, s_h.ds_subscribed ? "on" : "off");
    ESP_LOGI(TAG, "CTS svc=%u-%u chr=%u cccd=%u; service_changed=%u", s_cts.svc_start,
             s_cts.svc_end, s_cts.chr_val, s_cts.cccd, s_service_changed_handle);
    ESP_LOGI(TAG, "ancs stats: events=%u complete=%u overflow=%u malformed=%u",
             (unsigned)s_ancs.stat_events, (unsigned)s_ancs.stat_attrs_complete,
             (unsigned)s_ancs.stat_ds_overflow, (unsigned)s_ancs.stat_ds_malformed);
    for (unsigned i = 0; i < ANCS_MAX_TRACKED; i++) {
        if (s_ancs.tracked[i].used) {
            ESP_LOGI(TAG, "  uid=%u category=%u flags=0x%02x", (unsigned)s_ancs.tracked[i].uid,
                     s_ancs.tracked[i].category, s_ancs.tracked[i].event_flags);
        }
    }
}

#else

void apple_client_init(const apple_client_cfg_t *cfg) { (void)cfg; }
void apple_client_on_connect(uint16_t conn_handle) { (void)conn_handle; }
void apple_client_on_disconnect(void) {}
void apple_client_on_encryption(uint16_t conn_handle, int status) {
    (void)conn_handle;
    (void)status;
}
bool apple_client_on_notify_rx(struct ble_gap_event *event) {
    (void)event;
    return false;
}
void apple_client_tick(uint32_t now_ms) { (void)now_ms; }
bool apple_client_perform_action(uint32_t uid, ancs_action_id_t action) {
    (void)uid;
    (void)action;
    return false;
}
bool apple_client_ancs_ready(void) { return false; }
// No discovery will ever run, so there is nothing left to wait for -- which is what
// stops main.c logging an empty service table forever.
bool apple_client_may_retry(void) { return false; }
void apple_client_dump(void) {}

#endif
