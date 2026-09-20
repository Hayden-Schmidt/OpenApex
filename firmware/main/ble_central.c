#include "ble_central.h"

#include "packet.h"

#include "esp_log.h"
#include "nvs_flash.h"

#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include <string.h>

static const char *TAG = "ble_central";

// Fixed OpenApex BLE identifiers (see docs/OpenApex_SPEC.md §5.3), byte-reversed for
// BLE_UUID128_INIT (which takes the wire/little-endian byte order).
static const ble_uuid128_t s_svc_uuid =
    BLE_UUID128_INIT(0x4f, 0x3e, 0x2d, 0x1a, 0x6b, 0x2f, 0x8e, 0x9c, 0x0a, 0x4f, 0x01, 0x00, 0xa0, 0xd0, 0xc6, 0xc9);
static const ble_uuid128_t s_chr_uuid =
    BLE_UUID128_INIT(0x4f, 0x3e, 0x2d, 0x1a, 0x6b, 0x2f, 0x8e, 0x9c, 0x0a, 0x4f, 0x02, 0x00, 0xa0, 0xd0, 0xc6, 0xc9);

#define CCCD_UUID16 0x2902
#define DESIRED_MTU 185

static QueueHandle_t s_raw_packet_queue;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_svc_start_handle;
static uint16_t s_svc_end_handle;
static uint16_t s_chr_val_handle;
static uint8_t s_own_addr_type;

static void start_scan(void);

static int cccd_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg) {
    (void)conn_handle;
    (void)attr;
    (void)arg;
    if (error->status != 0) {
        ESP_LOGW(TAG, "CCCD write failed; status=%d", error->status);
    } else {
        ESP_LOGI(TAG, "subscribed to nav characteristic");
    }
    return 0;
}

static int disc_dsc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                        uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg) {
    (void)chr_val_handle;
    (void)arg;
    if (error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "descriptor discovery failed; status=%d", error->status);
        return 0;
    }
    if (dsc != NULL && ble_uuid_u16(&dsc->uuid.u) == CCCD_UUID16) {
        uint8_t value[2] = {0x01, 0x00}; // enable notifications
        ble_gattc_write_flat(conn_handle, dsc->handle, value, sizeof(value), cccd_write_cb, NULL);
    }
    return 0;
}

static int disc_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                        const struct ble_gatt_chr *chr, void *arg) {
    (void)arg;
    if (error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "characteristic discovery failed; status=%d", error->status);
        return 0;
    }
    if (chr != NULL && ble_uuid_cmp(&chr->uuid.u, &s_chr_uuid.u) == 0) {
        s_chr_val_handle = chr->val_handle;
        ESP_LOGI(TAG, "found nav characteristic, val_handle=%u", s_chr_val_handle);
        ble_gattc_disc_all_dscs(conn_handle, s_chr_val_handle, s_svc_end_handle, disc_dsc_cb, NULL);
    }
    return 0;
}

static int disc_svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                        const struct ble_gatt_svc *service, void *arg) {
    (void)arg;
    if (error->status == 0 && service != NULL) {
        s_svc_start_handle = service->start_handle;
        s_svc_end_handle = service->end_handle;
        ESP_LOGI(TAG, "found nav service, handles=%u..%u", s_svc_start_handle, s_svc_end_handle);
        ble_gattc_disc_all_chrs(conn_handle, s_svc_start_handle, s_svc_end_handle, disc_chr_cb, NULL);
    } else if (error->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "service discovery failed; status=%d, disconnecting", error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int mtu_cb(uint16_t conn_handle, const struct ble_gatt_error *error, uint16_t mtu, void *arg) {
    (void)arg;
    if (error->status != 0) {
        ESP_LOGW(TAG, "MTU exchange failed; status=%d, continuing at default MTU", error->status);
    } else {
        ESP_LOGI(TAG, "negotiated MTU=%u", mtu);
    }
    // Proceed with discovery regardless of MTU outcome; the relay/decoder still function at a
    // smaller MTU as long as it is >= RAW_NOTIF_PACKET_SIZE + ATT overhead.
    ble_gattc_disc_svc_by_uuid(conn_handle, &s_svc_uuid.u, disc_svc_cb, NULL);
    return 0;
}

static void handle_notification(const struct os_mbuf *om) {
    uint8_t buf[RAW_NOTIF_PACKET_SIZE];
    uint16_t out_len = 0;
    if (ble_hs_mbuf_to_flat((struct os_mbuf *)om, buf, sizeof(buf), &out_len) != 0) {
        return;
    }
    raw_notif_t raw;
    if (!packet_decode(buf, out_len, &raw)) {
        ESP_LOGW(TAG, "dropped malformed/unsupported-version packet (len=%u)", out_len);
        return;
    }
    ESP_LOGI(TAG, "decoded packet seq=%lu title=\"%s\" dist=\"%s\" speed_x10=%u heading=%u",
             (unsigned long)raw.sequence, raw.title_str, raw.distance_str, raw.speed_kmh_x10, raw.heading_deg);
    // ble_handler_task must never block on a full queue beyond a short bounded wait; drop the
    // packet rather than stall the NimBLE host task if countdown_task has fallen behind.
    if (xQueueSend(s_raw_packet_queue, &raw, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGW(TAG, "raw_packet_queue full, dropping packet seq=%lu", (unsigned long)raw.sequence);
    }
}

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0) {
            return 0;
        }
        for (int i = 0; i < fields.num_uuids128; i++) {
            if (ble_uuid_cmp(&fields.uuids128[i].u, &s_svc_uuid.u) == 0) {
                ble_gap_disc_cancel();
                ble_gap_connect(s_own_addr_type, &event->disc.addr, 30000, NULL, ble_gap_event_cb, NULL);
                break;
            }
        }
        return 0;
    }
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "connected to relay, conn_handle=%u", s_conn_handle);
            ble_gattc_exchange_mtu(s_conn_handle, mtu_cb, NULL);
        } else {
            ESP_LOGW(TAG, "connect failed; status=%d, resuming scan", event->connect.status);
            start_scan();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected from relay; reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_chr_val_handle = 0;
        start_scan();
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event->notify_rx.attr_handle == s_chr_val_handle) {
            handle_notification(event->notify_rx.om);
        }
        return 0;
    default:
        return 0;
    }
}

static void start_scan(void) {
    struct ble_gap_disc_params disc_params;
    memset(&disc_params, 0, sizeof(disc_params));
    disc_params.passive = 1;
    disc_params.filter_duplicates = 1;
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &disc_params, ble_gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_disc failed; rc=%d", rc);
    }
}

static void on_sync(void) {
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    start_scan();
}

static void on_reset(int reason) {
    ESP_LOGW(TAG, "NimBLE host reset; reason=%d", reason);
}

static void host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_central_init(QueueHandle_t raw_packet_queue) {
    s_raw_packet_queue = raw_packet_queue;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    // No ble_svc_gap_init()/ble_svc_gatt_init() here: those register the GAP/GATT *server*
    // services and compile out entirely with BLE_GATTS/CONFIG_BT_NIMBLE_GAP_SERVICE disabled,
    // which is correct for a central-only role — the terminal is a GATT client, not a server.
    nimble_port_freertos_init(host_task);
}
