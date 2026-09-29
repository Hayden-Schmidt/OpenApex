#include "ble_link.h"

#include "call_packet.h"
#include "apple_client.h"
#include "avatar_store.h"
#include "board_profile.h"
#include "drive_log.h"
#include "mesh.h"
#include "packet.h"
#include "settings.h"

#include "esp_app_desc.h"
#include "esp_log.h"

#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <string.h>

static const char *TAG = "ble_link";

// Fixed OpenApex BLE identifiers (see docs/reference/ble_link.md), byte-reversed for
// BLE_UUID128_INIT (which takes the wire/little-endian byte order).
static const ble_uuid128_t s_svc_uuid =
    BLE_UUID128_INIT(0x4f, 0x3e, 0x2d, 0x1a, 0x6b, 0x2f, 0x8e, 0x9c, 0x0a, 0x4f, 0x01, 0x00, 0xa0, 0xd0, 0xc6, 0xc9);
static const ble_uuid128_t s_chr_uuid =
    BLE_UUID128_INIT(0x4f, 0x3e, 0x2d, 0x1a, 0x6b, 0x2f, 0x8e, 0x9c, 0x0a, 0x4f, 0x02, 0x00, 0xa0, 0xd0, 0xc6, 0xc9);

// Incoming-call characteristics (call_packet.h): CALL_EVENT is written by the phone, CALL_ACTION
// notifies the phone when the rider taps Accept.
static const ble_uuid128_t s_call_event_uuid =
    BLE_UUID128_INIT(0x4f, 0x3e, 0x2d, 0x1a, 0x6b, 0x2f, 0x8e, 0x9c, 0x0a, 0x4f, 0x03, 0x00, 0xa0, 0xd0, 0xc6, 0xc9);
static const ble_uuid128_t s_call_action_uuid =
    BLE_UUID128_INIT(0x4f, 0x3e, 0x2d, 0x1a, 0x6b, 0x2f, 0x8e, 0x9c, 0x0a, 0x4f, 0x04, 0x00, 0xa0, 0xd0, 0xc6, 0xc9);
// Settings (§5.6): read the full TLV, write a partial one. Device info: read-only capabilities the
// phone needs before it can build a settings screen or rasterise an avatar. Avatar: 80 B of A2 art.
static const ble_uuid128_t s_settings_uuid =
    BLE_UUID128_INIT(0x4f, 0x3e, 0x2d, 0x1a, 0x6b, 0x2f, 0x8e, 0x9c, 0x0a, 0x4f, 0x05, 0x00, 0xa0, 0xd0, 0xc6, 0xc9);
static const ble_uuid128_t s_info_uuid =
    BLE_UUID128_INIT(0x4f, 0x3e, 0x2d, 0x1a, 0x6b, 0x2f, 0x8e, 0x9c, 0x0a, 0x4f, 0x06, 0x00, 0xa0, 0xd0, 0xc6, 0xc9);
static const ble_uuid128_t s_avatar_uuid =
    BLE_UUID128_INIT(0x4f, 0x3e, 0x2d, 0x1a, 0x6b, 0x2f, 0x8e, 0x9c, 0x0a, 0x4f, 0x07, 0x00, 0xa0, 0xd0, 0xc6, 0xc9);
// Rider mesh (§5.7): the phone's rider identity, and the friend list with its completion notify.
static const ble_uuid128_t s_identity_uuid =
    BLE_UUID128_INIT(0x4f, 0x3e, 0x2d, 0x1a, 0x6b, 0x2f, 0x8e, 0x9c, 0x0a, 0x4f, 0x08, 0x00, 0xa0, 0xd0, 0xc6, 0xc9);
static const ble_uuid128_t s_friends_uuid =
    BLE_UUID128_INIT(0x4f, 0x3e, 0x2d, 0x1a, 0x6b, 0x2f, 0x8e, 0x9c, 0x0a, 0x4f, 0x09, 0x00, 0xa0, 0xd0, 0xc6, 0xc9);

#define DEVICE_INFO_VERSION 1u
#define DEVICE_INFO_CAP_AVATAR 0x01u
// The nav characteristic also accepts a whole 210-byte packet in one write. A phone that sees
// this bit and has the MTU for it can skip fragmenting; one that does not, or cannot, keeps
// fragmenting and is served identically.
#define DEVICE_INFO_CAP_UNFRAGMENTED 0x02u
// This terminal reads ANCS and CTS off an iPhone itself, so the app need not (and on iOS
// cannot) relay notifications or the clock.
#define DEVICE_INFO_CAP_ANCS 0x04u

#if defined(BOARD_PROFILE_PROTOTYPE_C3_GC9A01)
#define DEVICE_INFO_BOARD_ID 1u
#elif defined(BOARD_PROFILE_S3_AMOLED_175)
#define DEVICE_INFO_BOARD_ID 2u
#endif

static QueueHandle_t s_raw_packet_queue;
static QueueHandle_t s_call_event_queue;
static uint16_t s_chr_val_handle;
static uint16_t s_call_event_val_handle;
static uint16_t s_call_action_val_handle;
// Current link and whether the phone has subscribed to CALL_ACTION. Written from the NimBLE host
// task, read by ble_link_send_call_accept() from countdown_task; aligned single-word stores.
static volatile uint16_t s_conn_handle;
static volatile bool s_call_action_subscribed;
static uint16_t s_friends_val_handle;
static volatile bool s_friends_subscribed;
static uint8_t s_own_addr_type;

// ANCS service UUID, byte-reversed for BLE_UUID128_INIT. We do not serve this -- we SOLICIT it:
// an advert carrying a solicitation tells iOS "this accessory wants your notification service",
// which is what makes the terminal pairable from iOS Settings and gets ANCS published to us once
// bonded (docs/reference/ios_ancs.md). apple_client.c is the consumer.
static const ble_uuid128_t s_ancs_sol_uuid =
    BLE_UUID128_INIT(0xd0, 0x00, 0x2d, 0x12, 0x1e, 0x4b, 0x0f, 0xa4, 0x99, 0x4e, 0xce, 0xb5, 0x31,
                     0xf4, 0x05, 0x79);

// Advertising rates, in 0.625ms units. Fast is the old always-on rate, kept for the window where
// a phone is plausibly looking; slow is 152.5ms, the slowest of Apple's recommended set and the
// floor of what they consider acceptable for sustained advertising.
#define OPENAPEX_ADV_FAST_ITVL_MIN 32u  // 20ms
#define OPENAPEX_ADV_FAST_ITVL_MAX 64u  // 40ms
#define OPENAPEX_ADV_SLOW_ITVL_MIN 244u // 152.5ms
#define OPENAPEX_ADV_SLOW_ITVL_MAX 400u // 250ms
#define OPENAPEX_ADV_FAST_MS 30000u     // fast burst on every (re)start

#define PHONE_ADV_INSTANCE 0
// False during the post-(re)start fast burst, true once it has elapsed. Written only from the
// NimBLE host task.
static bool s_adv_slow;
static bool s_adv_configured; // instance 0 holds usable params, so a failed re-configure is survivable
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

// Call events use the same fragment header on their own characteristic, with their own
// reassembly so an interleaved nav packet never corrupts a ring (or the other way round).
#define CALL_FRAG_COUNT ((CALL_EVENT_PACKET_SIZE + BLE_FRAG_CHUNK_BYTES - 1U) / BLE_FRAG_CHUNK_BYTES)
static uint8_t s_call_reasm_buf[CALL_EVENT_PACKET_SIZE];
static uint8_t s_call_reasm_packet_id;
static uint8_t s_call_reasm_have_mask;
static bool s_call_reasm_active;

// Whether the link is currently unusable: nothing has decoded for a long run of writes. The GUI
// reads this to show a fault instead of a frozen last-good screen.
bool ble_link_is_faulted(void) {
    return s_consecutive_decode_failures >= BLE_LINK_FAULT_THRESHOLD;
}

// Written only from the NimBLE host task's GAP callback, read from countdown_task. A single
// aligned bool store/load is atomic on the C3, so no lock.
static volatile bool s_connected;

bool ble_link_is_connected(void) {
    return s_connected;
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
    s_call_reasm_active = false;
    s_call_reasm_have_mask = 0;
}

static int call_event_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint8_t buf[BLE_FRAG_HEADER_BYTES + BLE_FRAG_CHUNK_BYTES];
    uint16_t out_len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &out_len) != 0 || out_len < BLE_FRAG_HEADER_BYTES) {
        ESP_LOGW(TAG, "call event: short write (len=%u)", out_len);
        return 0;
    }
    const uint8_t packet_id = buf[0];
    const uint8_t frag_index = buf[1];
    const uint8_t frag_count = buf[2];
    if (frag_count != CALL_FRAG_COUNT || frag_index >= frag_count) {
        ESP_LOGW(TAG, "call event: bad framing %u/%u", frag_index, frag_count);
        return 0;
    }
    const size_t expected_chunk = (frag_index == frag_count - 1)
                                      ? (CALL_EVENT_PACKET_SIZE - (size_t)frag_index * BLE_FRAG_CHUNK_BYTES)
                                      : BLE_FRAG_CHUNK_BYTES;
    const size_t got_chunk = (size_t)out_len - BLE_FRAG_HEADER_BYTES;
    if (got_chunk != expected_chunk) {
        ESP_LOGW(TAG, "call event: fragment %u is %u bytes, need %u", frag_index, (unsigned)got_chunk,
                 (unsigned)expected_chunk);
        return 0;
    }
    if (!s_call_reasm_active || packet_id != s_call_reasm_packet_id) {
        s_call_reasm_active = true;
        s_call_reasm_packet_id = packet_id;
        s_call_reasm_have_mask = 0;
    }
    memcpy(&s_call_reasm_buf[(size_t)frag_index * BLE_FRAG_CHUNK_BYTES], &buf[BLE_FRAG_HEADER_BYTES], got_chunk);
    s_call_reasm_have_mask = (uint8_t)(s_call_reasm_have_mask | (1u << frag_index));
    if (s_call_reasm_have_mask != (uint8_t)((1u << frag_count) - 1u)) {
        return 0;
    }
    s_call_reasm_active = false;
    s_call_reasm_have_mask = 0;

    call_event_t event;
    if (!call_event_decode(s_call_reasm_buf, CALL_EVENT_PACKET_SIZE, &event)) {
        ESP_LOGW(TAG, "call event: dropped malformed/unsupported-version packet");
        return 0;
    }
    ESP_LOGI(TAG, "call event type=%d id=%u caller=\"%s\"", (int)event.type, event.call_id, event.caller);
    if (xQueueSend(s_call_event_queue, &event, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGW(TAG, "call_event_queue full, dropping call event id=%u", event.call_id);
    }
    return 0;
}

// CALL_ACTION is notify-only; nothing reads it.
static int call_action_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

bool ble_link_send_call_accept(uint16_t call_id) {
    if (!s_connected || !s_call_action_subscribed) {
        ESP_LOGW(TAG, "call accept id=%u not sent: connected=%d subscribed=%d", call_id, s_connected,
                 s_call_action_subscribed);
        return false;
    }
    uint8_t payload[CALL_ACTION_PACKET_SIZE];
    call_action_encode(CALL_ACTION_ACCEPT, call_id, payload);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(payload, sizeof(payload));
    if (om == NULL) {
        return false;
    }
    // ble_gatts_notify_custom consumes om whether or not it succeeds.
    const int rc = ble_gatts_notify_custom(s_conn_handle, s_call_action_val_handle, om);
    ESP_LOGI(TAG, "call accept id=%u notify rc=%d", call_id, rc);
    return rc == 0;
}

static int deliver_reassembled(void);

static int chr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    // Single-write fast path. The 13-fragment scheme below exists for an Android stack that
    // truncated every write to 20 bytes regardless of the MTU it had just agreed; iOS
    // negotiates 185 and honours it, so there the fragmenting costs 12 extra writes per packet
    // to work around a bug that is not present.
    //
    // The two framings are told apart by length rather than by a version field in the header:
    // a fragment write is at most BLE_FRAG_HEADER_BYTES + BLE_FRAG_CHUNK_BYTES == 20 bytes, so
    // a 210-byte write cannot be one, and the packet's own version byte already covers format
    // changes. Nothing is negotiated and a writer may switch per packet -- which matters
    // because the MTU is only known after the exchange, and on Android it lies.
    if (OS_MBUF_PKTLEN(ctxt->om) == RAW_NOTIF_PACKET_SIZE) {
        uint16_t whole_len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, s_reasm_buf, sizeof(s_reasm_buf), &whole_len) != 0 ||
            whole_len != RAW_NOTIF_PACKET_SIZE) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        // A whole packet supersedes any half-finished fragmented one, and it has just
        // overwritten the buffer that one was accumulating into.
        reasm_reset();
        return deliver_reassembled();
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
    return deliver_reassembled();
}

// Decodes whatever is in s_reasm_buf and hands it to countdown_task. Shared by the fragment and
// single-write paths, which differ only in how the 210 bytes got there.
static int deliver_reassembled(void) {
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

static int settings_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    uint8_t buf[SETTINGS_TLV_MAX_LEN + 16]; // room for a newer phone's extra tags
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const size_t n = settings_tlv_encode(settings_get(), buf, sizeof(buf));
        return os_mbuf_append(ctxt->om, buf, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    // Copy-on-success: a rejected write changes nothing, and the phone sees the ATT error.
    terminal_settings_t next = *settings_get();
    if (!settings_tlv_decode(buf, len, &next)) {
        ESP_LOGW(TAG, "rejected settings write (len=%u)", len);
        return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    }
    settings_set(&next);
    return 0;
}

static int info_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    const esp_app_desc_t *app = esp_app_get_description();
    const uint8_t head[] = {
        DEVICE_INFO_VERSION,
        DEVICE_INFO_BOARD_ID,
        BOARD_GFX_TIER,
        SETTINGS_SCHEMA_VERSION,
        DEVICE_INFO_CAP_AVATAR | DEVICE_INFO_CAP_UNFRAGMENTED |
            (BOARD_HAS_ANCS ? DEVICE_INFO_CAP_ANCS : 0u),
        (uint8_t)(BOARD_AVATAR_PX & 0xFF),
        (uint8_t)(BOARD_AVATAR_PX >> 8),
    };
    if (os_mbuf_append(ctxt->om, head, sizeof(head)) != 0 ||
        os_mbuf_append(ctxt->om, app->version, strnlen(app->version, sizeof(app->version))) != 0) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return 0;
}

static int avatar_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    return avatar_store_ble_access(ctxt);
}

static int identity_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    return mesh_identity_ble_access(ctxt);
}

static int friends_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    return mesh_friends_ble_access(ctxt);
}

bool ble_link_send_friend(const uint8_t *buf, uint16_t len) {
    if (!s_connected || !s_friends_subscribed) return false;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, len);
    if (om == NULL) return false;
    const int rc = ble_gatts_notify_custom(s_conn_handle, s_friends_val_handle, om);
    ESP_LOGI(TAG, "friend notify rc=%d", rc);
    return rc == 0;
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
                // nav packets. No response needed â€” the phone re-publishes on every state change,
                // so a dropped write is superseded by the next one rather than retried.
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
                .val_handle = &s_chr_val_handle,
            },
            {
                .uuid = &s_call_event_uuid.u,
                .access_cb = call_event_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
                .val_handle = &s_call_event_val_handle,
            },
            {
                // Notify only. READ_ENC keeps the CCCD write (the phone's subscription) on the
                // encrypted, bonded link like everything else here.
                .uuid = &s_call_action_uuid.u,
                .access_cb = call_action_access_cb,
                .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
                .val_handle = &s_call_action_val_handle,
            },
            {
                // Same bonded-only rule as nav packets: settings and the avatar are the rider's.
                // Writes are acknowledged so the app can show sync state (Phone_App_Architecture.md
                // §7) and a rejected value surfaces as an ATT error.
                .uuid = &s_settings_uuid.u,
                .access_cb = settings_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                .uuid = &s_info_uuid.u,
                .access_cb = info_access_cb,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = &s_avatar_uuid.u,
                .access_cb = avatar_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                // Holds the rider key, so bonded-only both ways.
                .uuid = &s_identity_uuid.u,
                .access_cb = identity_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                .uuid = &s_friends_uuid.u,
                .access_cb = friends_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_friends_val_handle,
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
            s_conn_handle = event->connect.conn_handle;
            s_call_action_subscribed = false;
            s_friends_subscribed = false;
            s_connected = true;
            apple_client_on_connect(event->connect.conn_handle);
            // Ask for encryption rather than waiting to be asked. Every ANCS characteristic
            // requires an encrypted link, and the old behaviour -- let the central retry after an
            // ATT insufficient-authentication error -- only happens if the central writes
            // something first. On iOS nothing writes: the phone is the server on that path, so
            // without this the bond never forms and ANCS is never published.
            {
                struct ble_gap_conn_desc desc;
                if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0 && !desc.sec_state.encrypted) {
                    const int src = ble_gap_security_initiate(event->connect.conn_handle);
                    if (src != 0 && src != BLE_HS_EALREADY) {
                        ESP_LOGW(TAG, "security_initiate failed; rc=%d", src);
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "connect failed; status=%d, resuming advertising", event->connect.status);
            start_advertising();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "central disconnected; reason=%d", event->disconnect.reason);
        drive_log_ble(DRIVE_LOG_BLE_DISCONNECTED, (int32_t)event->disconnect.reason);
        s_connected = false;
        s_call_action_subscribed = false;
        s_friends_subscribed = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        apple_client_on_disconnect();
        // A phone has just gone away, so this is exactly when one is likely to come looking.
        s_adv_slow = false;
        start_advertising();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_call_action_val_handle) {
            s_call_action_subscribed = event->subscribe.cur_notify != 0;
            ESP_LOGI(TAG, "call action notifications %s", s_call_action_subscribed ? "on" : "off");
        }
        if (event->subscribe.attr_handle == s_friends_val_handle) {
            s_friends_subscribed = event->subscribe.cur_notify != 0;
        }
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        // reason == 0 means advertising stopped because a central connected, and NimBLE delivers
        // this BEFORE the CONNECT event. Restarting here would take the one connection slot's
        // pool entry into ble_gap_ext_adv_configure(), which rejects a connectable advert with
        // ENOMEM when none is free -- a confusing rc=6 in the log for a thing we did not want to
        // do anyway. The CONNECT/DISCONNECT pair owns advertising across a connection.
        if (event->adv_complete.reason == 0) {
            return 0;
        }
        // Otherwise the fast burst elapsed (the only thing that sets a duration). Drop to the
        // Apple-compliant rate and keep advertising indefinitely.
        s_adv_slow = true;
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
    case BLE_GAP_EVENT_ENC_CHANGE:
        // The bond is up (or failed). This is the gate for the whole iOS path: ANCS and CTS
        // discovery starts here, never on connect.
        {
            struct ble_gap_conn_desc desc;
            const bool enc = ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0 &&
                             desc.sec_state.encrypted;
            ESP_LOGI(TAG, "encryption change: status=%d encrypted=%d", event->enc_change.status,
                     (int)enc);
            apple_client_on_encryption(event->enc_change.conn_handle,
                                       enc ? 0 : (event->enc_change.status != 0
                                                      ? event->enc_change.status
                                                      : -1));
        }
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX:
        // Inbound notifications/indications exist only because we are also a GATT client here:
        // ANCS Notification Source and Data Source, CTS, and GATT Service Changed.
        (void)apple_client_on_notify_rx(event);
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

// Fills an mbuf with advertising fields and hands it to NimBLE. Returns 0 on success; the mbuf
// is consumed either way.
static int set_adv_payload(const struct ble_hs_adv_fields *fields, bool scan_rsp) {
    struct os_mbuf *data = os_msys_get_pkthdr(BLE_HS_ADV_MAX_SZ, 0);
    if (data == NULL) {
        ESP_LOGW(TAG, "no mbuf for advertising data");
        return -1;
    }
    int rc = ble_hs_adv_set_fields_mbuf(fields, data);
    if (rc != 0) {
        // Almost always "does not fit in 31 bytes". Say so loudly: a silently dropped
        // solicitation is an iPhone that never offers to pair, with nothing on screen to explain it.
        ESP_LOGE(TAG, "advert payload (%s) rejected; rc=%d", scan_rsp ? "scan rsp" : "primary", rc);
        os_mbuf_free_chain(data);
        return rc;
    }
    rc = scan_rsp ? ble_gap_ext_adv_rsp_set_data(PHONE_ADV_INSTANCE, data)   // consumes data
                  : ble_gap_ext_adv_set_data(PHONE_ADV_INSTANCE, data);
    if (rc != 0) {
        ESP_LOGW(TAG, "advertising data failed; rc=%d", rc);
    }
    return rc;
}

static void start_advertising(void) {
    // The 31-byte legacy payload cannot hold flags (3B) + our service UUID128 (18B) + the ANCS
    // solicitation (18B) + a name (10B) -- that is 49 bytes. So it splits:
    //
    //   primary     flags + our 128-bit service UUID   (21B)
    //   scan rsp    ANCS solicitation + "OpenApex"     (28B)
    //
    // Our service UUID stays in the PRIMARY advert deliberately: Android's CompanionDeviceManager
    // observer mode filters on it there, and that path is already fragile enough
    // (docs/investigations/CDM_Cold_Wake_Investigation.md). The name moves to the scan response, where an active
    // scanner -- which the CDM picker is -- still reads it.
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&s_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.sol_uuids128 = &s_ancs_sol_uuid;
    rsp.sol_num_uuids128 = 1;
    rsp.name = (const uint8_t *)"OpenApex";
    rsp.name_len = 8;
    rsp.name_is_complete = 1;

    // Extended-advertising API (the rider mesh owns instance 1, mesh_transport_ble.c), but a
    // legacy connectable PDU on 1M, so the phone sees exactly the advert it always has.
    struct ble_gap_ext_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.connectable = 1;
    adv_params.scannable = 1;
    adv_params.legacy_pdu = 1;
    adv_params.own_addr_type = s_own_addr_type;
    adv_params.primary_phy = BLE_HCI_LE_PHY_1M;
    adv_params.secondary_phy = BLE_HCI_LE_PHY_1M;
    adv_params.sid = PHONE_ADV_INSTANCE;
    // Advertising interval. 20-40ms was chosen for Android's low-power background scan (used by
    // CompanionDeviceManager observer mode), which samples the air infrequently enough that a slow
    // advertiser gets missed for several scan windows in a row.
    //
    // Apple's Accessory Design Guidelines want 152.5ms or slower for SUSTAINED advertising, and a
    // 20ms advertiser is both out of spec and expensive on a battery. The compromise is a fast
    // burst on every (re)start, which is when a phone is actually looking, backing off to a
    // compliant steady rate -- see the duration/backoff on ble_gap_ext_adv_start below.
    adv_params.itvl_min = OPENAPEX_ADV_FAST_ITVL_MIN;
    adv_params.itvl_max = OPENAPEX_ADV_FAST_ITVL_MAX;
    if (s_adv_slow) {
        adv_params.itvl_min = OPENAPEX_ADV_SLOW_ITVL_MIN;
        adv_params.itvl_max = OPENAPEX_ADV_SLOW_ITVL_MAX;
    }
    // One connection slot (CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1), so a connectable advert cannot be
    // configured while a phone is on the link: ble_gap_ext_adv_params_validate() rejects it with
    // ENOMEM. Nothing should call us in that state, but say so rather than emit a puzzling rc=6.
    if (s_connected) {
        ESP_LOGD(TAG, "not advertising: already connected");
        return;
    }
    // Reconfiguring a running instance is rejected too. Stop first; a no-op when it is idle.
    if (ble_gap_ext_adv_active(PHONE_ADV_INSTANCE)) {
        (void)ble_gap_ext_adv_stop(PHONE_ADV_INSTANCE);
    }
    int rc = ble_gap_ext_adv_configure(PHONE_ADV_INSTANCE, &adv_params, NULL, gap_event_cb, NULL);
    if (rc != 0) {
        // Only fatal the first time. Once the instance holds good params, a failed re-configure
        // costs the new interval, not the advert -- and returning here is what previously left
        // the terminal silently unadvertised, invisible to the phone until the next reboot.
        ESP_LOGW(TAG, "ble_gap_ext_adv_configure failed; rc=%d", rc);
        if (!s_adv_configured) {
            return;
        }
        ESP_LOGW(TAG, "re-advertising with the previous parameters");
    } else {
        s_adv_configured = true;
    }
    if (set_adv_payload(&fields, false) != 0) {
        return;
    }
    if (set_adv_payload(&rsp, true) != 0) {
        // Losing the scan response costs the solicitation and the name, not the link: Android
        // still sees the service UUID in the primary advert. Carry on advertising.
        ESP_LOGW(TAG, "advertising without ANCS solicitation");
    }
    // Fast phase runs for a bounded time, then ADV_COMPLETE brings us back here to restart slow.
    const int32_t duration_10ms = s_adv_slow ? 0 : (int32_t)(OPENAPEX_ADV_FAST_MS / 10);
    rc = ble_gap_ext_adv_start(PHONE_ADV_INSTANCE, duration_10ms, 0);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_ext_adv_start failed; rc=%d", rc);
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

void ble_link_init(QueueHandle_t raw_packet_queue, QueueHandle_t call_event_queue) {
    s_raw_packet_queue = raw_packet_queue;
    s_call_event_queue = call_event_queue;

    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    // Bonding: Just Works pairing (no display/input on the terminal to confirm a passkey),
    // requiring encryption on the nav characteristic write (BLE_GATT_CHR_F_WRITE_ENC above) so
    // only the bonded phone can push packets. Bond storage persists in NVS
    // (CONFIG_BT_NIMBLE_NVS_PERSIST) so the phone/terminal reconnect without re-pairing across
    // power cycles â€” required for the "bike on -> device boots -> phone reconnects" flow.
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
