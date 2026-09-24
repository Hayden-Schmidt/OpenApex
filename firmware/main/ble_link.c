#include "ble_link.h"

#include "drive_log.h"
#include "packet.h"

#include "esp_log.h"
#include "nvs_flash.h"

#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <string.h>

static const char *TAG = "ble_link";

// Fixed OpenApex BLE identifiers (see docs/OpenApex_SPEC.md §5.3), byte-reversed for
// BLE_UUID128_INIT (which takes the wire/little-endian byte order).
static const ble_uuid128_t s_svc_uuid =
    BLE_UUID128_INIT(0x4f, 0x3e, 0x2d, 0x1a, 0x6b, 0x2f, 0x8e, 0x9c, 0x0a, 0x4f, 0x01, 0x00, 0xa0, 0xd0, 0xc6, 0xc9);
static const ble_uuid128_t s_chr_uuid =
    BLE_UUID128_INIT(0x4f, 0x3e, 0x2d, 0x1a, 0x6b, 0x2f, 0x8e, 0x9c, 0x0a, 0x4f, 0x02, 0x00, 0xa0, 0xd0, 0xc6, 0xc9);

static QueueHandle_t s_raw_packet_queue;
static uint16_t s_chr_val_handle;
static uint8_t s_own_addr_type;

static void start_advertising(void);

// Consecutive failed decodes on the current link. Reset by any successful decode.
static uint32_t s_consecutive_decode_failures;
static uint16_t s_negotiated_mtu;

// Fragment framing for the nav characteristic.
//
// The 2026-09-24 captures showed the central's ATT MTU exchange cannot be relied on: Android
// reported GATT_REQUEST_NOT_SUPPORTED on every connection of a whole session while this side kept
// negotiating MTU=256, and every write still arrived here truncated to 20 bytes regardless. Rather
// than depend on that exchange for correctness, every write is now a small fixed-size fragment
// (3-byte header + up to 17 payload bytes = 20 bytes total) that fits inside the BLE-spec default
// 23-byte ATT MTU on every stack, negotiated or not. MTU is still logged as a diagnostic below, but
// nothing depends on it succeeding.
#define BLE_FRAG_HEADER_BYTES 3U
#define BLE_FRAG_CHUNK_BYTES 17U
#define BLE_FRAG_COUNT ((RAW_NOTIF_PACKET_SIZE + BLE_FRAG_CHUNK_BYTES - 1U) / BLE_FRAG_CHUNK_BYTES)
_Static_assert(BLE_FRAG_COUNT <= 16, "fragment count must fit the uint16_t received-mask");

static uint8_t s_reasm_buf[RAW_NOTIF_PACKET_SIZE];
static uint8_t s_reasm_packet_id;
static uint16_t s_reasm_have_mask;
static bool s_reasm_active;

// Whether the link is currently unusable: nothing has decoded for a long run of writes. The GUI
// reads this to show a fault instead of a frozen last-good screen.
bool ble_link_is_faulted(void) {
    return s_consecutive_decode_failures >= BLE_LINK_FAULT_THRESHOLD;
}

uint16_t ble_link_negotiated_mtu(void) {
    return s_negotiated_mtu;
}

/**
 * Records a failed decode, WITHOUT letting a broken link eat the log partition.
 *
 * On the 2026-09-24 capture a single stale connection wrote 6,565 consecutive DECODE_FAILED
 * records -- 80% of the 1 MB ring -- and destroyed the history of every earlier drive while
 * saying exactly one thing over and over. The information in the 6,565th identical failure is
 * zero. So: log the first few in full, then only on a change of length or a power-of-two
 * milestone, which keeps the shape of a long outage (when it started, how long it ran) at a cost
 * of a few dozen records instead of thousands.
 */
// expected_len is what a well-formed write at this point in the protocol should have been; a
// write shorter than that is a truncating link, not a corrupt/stale-format packet, and gets its
// own event (it has a different fix). Callers with no meaningful expectation (e.g. an invalid
// frag_count that isn't a length problem at all) pass expected_len == out_len to force DECODE_FAILED.
static void note_decode_failure(uint16_t out_len, uint16_t expected_len) {
    static uint16_t last_len;
    const uint32_t n = ++s_consecutive_decode_failures;

    const drive_log_ble_event_t event =
        (out_len < expected_len) ? DRIVE_LOG_BLE_TRUNCATED : DRIVE_LOG_BLE_DECODE_FAILED;

    const bool milestone = (n <= 3) || (out_len != last_len) || ((n & (n - 1)) == 0);
    last_len = out_len;
    if (!milestone) {
        return;
    }
    if (event == DRIVE_LOG_BLE_TRUNCATED) {
        ESP_LOGW(TAG, "truncated fragment write: got %u bytes, need %u (ATT MTU is %u) [%lu consecutive]",
                 out_len, expected_len, s_negotiated_mtu, (unsigned long)n);
    } else {
        ESP_LOGW(TAG, "dropped malformed/unsupported-version write (len=%u) [%lu consecutive]",
                 out_len, (unsigned long)n);
    }
    drive_log_ble(event, (int32_t)out_len);
}

static void reasm_reset(void) {
    s_reasm_active = false;
    s_reasm_have_mask = 0;
}

static int chr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint8_t buf[BLE_FRAG_HEADER_BYTES + BLE_FRAG_CHUNK_BYTES];
    uint16_t out_len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &out_len) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (out_len < BLE_FRAG_HEADER_BYTES) {
        note_decode_failure(out_len, BLE_FRAG_HEADER_BYTES);
        return 0;
    }

    const uint8_t packet_id = buf[0];
    const uint8_t frag_index = buf[1];
    const uint8_t frag_count = buf[2];
    // frag_count is a protocol constant, not a per-write value -- a mismatch means the relay is
    // running a different fragmentation scheme (stale build), which is not a length problem.
    if (frag_count != BLE_FRAG_COUNT || frag_index >= frag_count) {
        note_decode_failure(out_len, out_len);
        return 0;
    }

    const size_t expected_chunk = (frag_index == frag_count - 1)
                                       ? (RAW_NOTIF_PACKET_SIZE - (size_t)frag_index * BLE_FRAG_CHUNK_BYTES)
                                       : BLE_FRAG_CHUNK_BYTES;
    const size_t got_chunk = (size_t)out_len - BLE_FRAG_HEADER_BYTES;
    if (got_chunk != expected_chunk) {
        note_decode_failure(out_len, (uint16_t)(BLE_FRAG_HEADER_BYTES + expected_chunk));
        return 0;
    }

    // A new packet id (or no reassembly in progress) starts fresh. A dropped fragment then just
    // leaves a stale partial that free-runs until the next publish (~3Hz) supersedes it, rather
    // than jamming reassembly forever on one lost write.
    if (!s_reasm_active || packet_id != s_reasm_packet_id) {
        s_reasm_active = true;
        s_reasm_packet_id = packet_id;
        s_reasm_have_mask = 0;
    }
    memcpy(&s_reasm_buf[(size_t)frag_index * BLE_FRAG_CHUNK_BYTES], &buf[BLE_FRAG_HEADER_BYTES], got_chunk);
    s_reasm_have_mask = (uint16_t)(s_reasm_have_mask | (1u << frag_index));
    const uint16_t full_mask = (uint16_t)((1u << frag_count) - 1u);
    if (s_reasm_have_mask != full_mask) {
        return 0; // waiting on the rest of this packet's fragments
    }
    reasm_reset();

    raw_notif_t raw;
    if (!packet_decode(s_reasm_buf, RAW_NOTIF_PACKET_SIZE, &raw)) {
        note_decode_failure(RAW_NOTIF_PACKET_SIZE, RAW_NOTIF_PACKET_SIZE);
        return 0; // ATT-level success; the packet is simply discarded downstream
    }
    s_consecutive_decode_failures = 0;
    drive_log_raw(&raw);
    ESP_LOGI(TAG, "decoded packet seq=%lu title=\"%s\" dist=\"%s\" speed_x10=%u heading=%u",
             (unsigned long)raw.sequence, raw.title_str, raw.distance_str, raw.speed_kmh_x10, raw.heading_deg);
    // BLE callbacks must never block on a full queue beyond a short bounded wait; drop the
    // packet rather than stall the NimBLE host task if countdown_task has fallen behind.
    if (xQueueSend(s_raw_packet_queue, &raw, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGW(TAG, "raw_packet_queue full, dropping packet seq=%lu", (unsigned long)raw.sequence);
        drive_log_ble(DRIVE_LOG_BLE_QUEUE_FULL, (int32_t)raw.sequence);
    }
    return 0;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &s_chr_uuid.u,
                .access_cb = chr_access_cb,
                // Encrypted write required: only a bonded central (the Android relay) may push
                // nav packets. No response needed — the phone re-publishes on every state change,
                // so a dropped write is superseded by the next one rather than retried.
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
                .val_handle = &s_chr_val_handle,
            },
            {0}, // terminator
        },
    },
    {0}, // terminator
};

static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "central connected, conn_handle=%u", event->connect.conn_handle);
            drive_log_ble(DRIVE_LOG_BLE_CONNECTED, (int32_t)event->connect.conn_handle);
            // Fresh link: the previous connection's MTU, failure run and any in-flight fragment
            // reassembly say nothing about it.
            s_consecutive_decode_failures = 0;
            s_negotiated_mtu = BLE_ATT_MTU_DFLT;
            reasm_reset();
        } else {
            ESP_LOGW(TAG, "connect failed; status=%d, resuming advertising", event->connect.status);
            start_advertising();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "central disconnected; reason=%d", event->disconnect.reason);
        drive_log_ble(DRIVE_LOG_BLE_DISCONNECTED, (int32_t)event->disconnect.reason);
        start_advertising();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        return 0;
    case BLE_GAP_EVENT_MTU:
        // The exchange the whole packet path depends on. CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU is 256
        // here, so this reflects what the CENTRAL asked for; if it never asks, no event arrives and
        // the MTU stays at the 23-byte default, which silently truncates every 146-byte packet to
        // 20 bytes. Logged either way so a capture shows which happened instead of leaving it to be
        // inferred from the failure lengths.
        s_negotiated_mtu = event->mtu.value;
        ESP_LOGI(TAG, "ATT MTU negotiated: %u (need >= %u)", event->mtu.value,
                 (unsigned)(RAW_NOTIF_PACKET_SIZE + 3));
        drive_log_ble(DRIVE_LOG_BLE_MTU, (int32_t)event->mtu.value);
        if (event->mtu.value < RAW_NOTIF_PACKET_SIZE + 3) {
            ESP_LOGE(TAG, "ATT MTU %u is too small for a %u-byte packet; writes will be truncated",
                     event->mtu.value, (unsigned)RAW_NOTIF_PACKET_SIZE);
        }
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // A central we already have a bond for is re-pairing (e.g. it lost its bond). Delete the
        // stale bond and let the pairing procedure retry, rather than reject and get stuck.
        {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    default:
        return 0;
    }
}

static void start_advertising(void) {
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&s_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    // "OpenApex" (8 chars) is the longest name that fits alongside flags (3B) + UUID128 (18B) in
    // the 31-byte legacy primary payload (2B AD header + 8B = 10B, total 31B exactly). CDM/Android
    // only filters on the service UUID, but a real name replaces the raw MAC in the CDM picker UI.
    fields.name = (const uint8_t *)"OpenApex";
    fields.name_len = 8;
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_adv_set_fields failed; rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    // Fast advertising interval (20-40ms) rather than the NimBLE default (~1.28s): Android's
    // low-power background scan (used by CompanionDeviceManager observer mode) samples the air
    // infrequently, so a slow advertiser can be missed for multiple scan windows in a row.
    adv_params.itvl_min = 32; // 32 * 0.625ms = 20ms
    adv_params.itvl_max = 64; // 64 * 0.625ms = 40ms
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_adv_start failed; rc=%d", rc);
    }
}

static void on_sync(void) {
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    start_advertising();
}

static void on_reset(int reason) {
    ESP_LOGW(TAG, "NimBLE host reset; reason=%d", reason);
}

static void host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_link_init(QueueHandle_t raw_packet_queue) {
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

    // Bonding: Just Works pairing (no display/input on the terminal to confirm a passkey),
    // requiring encryption on the nav characteristic write (BLE_GATT_CHR_F_WRITE_ENC above) so
    // only the bonded phone can push packets. Bond storage persists in NVS
    // (CONFIG_BT_NIMBLE_NVS_PERSIST) so the phone/terminal reconnect without re-pairing across
    // power cycles — required for the "bike on -> device boots -> phone reconnects" flow.
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    ESP_ERROR_CHECK(rc == 0 ? ESP_OK : ESP_FAIL);
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    ESP_ERROR_CHECK(rc == 0 ? ESP_OK : ESP_FAIL);

    ble_svc_gap_device_name_set("OpenApex");

    nimble_port_freertos_init(host_task);
}
