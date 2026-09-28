#include "mesh_transport.h"

#include "mesh_proto.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include <string.h>

// Rider mesh over BLE extended advertising (§5.7). Advertising instance 0 belongs to ble_link.c
// (the phone's connectable advert); the mesh owns instance 1, non-connectable, on the Coded PHY for
// range. Each frame rides in manufacturer-specific data and is advertised for a short window
// before the next queued frame replaces it, since one advert carries one payload. Receiving is a
// passive Coded-PHY scan that never stops while the terminal is on, the phone link included.

static const char *TAG = "mesh_ble";

#define MESH_ADV_INSTANCE 1
#define MESH_COMPANY_ID 0xFFFFu // Bluetooth SIG "no company": test/internal use
#define ADV_ITVL 160u           // 100 ms in 0.625 ms units
#define FRAME_WINDOW_MS 300u    // ~3 adverts per frame, so a scanner catches at least one
#define SCAN_ITVL 160u          // 100 ms
#define SCAN_WINDOW 80u         // 50 ms: half duty, leaves air time for the phone link
#define OUTBOX_LEN 4u
#define AD_HEADER_LEN 4u        // AD length, AD type, company ID

typedef struct {
    uint8_t len;
    uint8_t data[MESH_FRAME_MAX];
} frame_t;

static mesh_rx_fn s_rx;
static SemaphoreHandle_t s_lock;
static frame_t s_outbox[OUTBOX_LEN];
static size_t s_head, s_count;
static bool s_configured;
static bool s_advertising;
static uint32_t s_frame_until_ms;

// -- receive (NimBLE host task) ------------------------------------------------------------------

static void on_report(const uint8_t *data, uint8_t len) {
    // Walk the AD structures for our manufacturer data.
    for (size_t i = 0; i + 1 < len;) {
        const uint8_t ad_len = data[i];
        if (ad_len == 0 || i + 1 + ad_len > len) return;
        const uint8_t *ad = &data[i + 1];
        if (ad[0] == BLE_HS_ADV_TYPE_MFG_DATA && ad_len > AD_HEADER_LEN - 1 &&
            (ad[1] | ad[2] << 8) == MESH_COMPANY_ID) {
            s_rx(&ad[3], (size_t)ad_len - 3u);
            return;
        }
        i += 1u + ad_len;
    }
}

static int gap_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    if (event->type == BLE_GAP_EVENT_EXT_DISC) {
        const struct ble_gap_ext_disc_desc *d = &event->ext_disc;
        if (d->data_status == BLE_GAP_EXT_ADV_DATA_STATUS_COMPLETE) on_report(d->data, d->length_data);
    }
    // DISC_COMPLETE / ADV_COMPLETE need nothing: poll() restarts whatever stopped.
    return 0;
}

// -- send (mesh task) ----------------------------------------------------------------------------

static bool configure(void) {
    struct ble_gap_ext_adv_params p;
    memset(&p, 0, sizeof(p));
    p.own_addr_type = BLE_OWN_ADDR_PUBLIC;
    p.primary_phy = BLE_HCI_LE_PHY_CODED;
    p.secondary_phy = BLE_HCI_LE_PHY_CODED;
    p.itvl_min = ADV_ITVL;
    p.itvl_max = ADV_ITVL;
    p.sid = MESH_ADV_INSTANCE;
    const int rc = ble_gap_ext_adv_configure(MESH_ADV_INSTANCE, &p, NULL, gap_cb, NULL);
    if (rc != 0) ESP_LOGW(TAG, "adv configure rc=%d", rc);
    return rc == 0;
}

static void advertise(const frame_t *f) {
    uint8_t ad[AD_HEADER_LEN + MESH_FRAME_MAX];
    ad[0] = (uint8_t)(3u + f->len);
    ad[1] = BLE_HS_ADV_TYPE_MFG_DATA;
    ad[2] = MESH_COMPANY_ID & 0xFFu;
    ad[3] = MESH_COMPANY_ID >> 8;
    memcpy(&ad[AD_HEADER_LEN], f->data, f->len);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(ad, (uint16_t)(AD_HEADER_LEN + f->len));
    if (om == NULL) return;
    if (s_advertising) ble_gap_ext_adv_stop(MESH_ADV_INSTANCE);
    s_advertising = false;
    int rc = ble_gap_ext_adv_set_data(MESH_ADV_INSTANCE, om); // consumes om
    if (rc == 0) rc = ble_gap_ext_adv_start(MESH_ADV_INSTANCE, 0, 0);
    if (rc != 0) ESP_LOGW(TAG, "advertise rc=%d", rc);
    s_advertising = rc == 0;
}

static void start_scan(void) {
    const struct ble_gap_ext_disc_params coded = {.itvl = SCAN_ITVL, .window = SCAN_WINDOW, .passive = 1};
    const int rc = ble_gap_ext_disc(BLE_OWN_ADDR_PUBLIC, 0, 0, 0, BLE_HCI_SCAN_FILT_NO_WL, 0, NULL, &coded,
                                    gap_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "scan rc=%d", rc);
}

static bool ble_start(mesh_rx_fn rx) {
    s_rx = rx;
    s_lock = xSemaphoreCreateMutex();
    return s_lock != NULL;
}

static void ble_send(const uint8_t *frame, size_t len) {
    if (len > MESH_FRAME_MAX || len > 251u - AD_HEADER_LEN) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_count == OUTBOX_LEN) { // drop the oldest: every frame type is re-sent periodically
        s_head = (s_head + 1u) % OUTBOX_LEN;
        s_count--;
    }
    frame_t *f = &s_outbox[(s_head + s_count) % OUTBOX_LEN];
    f->len = (uint8_t)len;
    memcpy(f->data, frame, len);
    s_count++;
    xSemaphoreGive(s_lock);
}

static void ble_poll(uint32_t now_ms) {
    // The host resyncs after a controller reset, dropping our advert and scan; until it syncs
    // there is nothing to drive. Checking every pass recovers without a hook into ble_link.c.
    if (!ble_hs_synced()) {
        s_configured = false;
        s_advertising = false;
        return;
    }
    if (!s_configured) s_configured = configure();
    if (!ble_gap_disc_active()) start_scan();
    if (!s_configured || (s_advertising && (int32_t)(now_ms - s_frame_until_ms) < 0)) return;

    frame_t f;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool have = s_count > 0;
    if (have) {
        f = s_outbox[s_head];
        s_head = (s_head + 1u) % OUTBOX_LEN;
        s_count--;
    }
    xSemaphoreGive(s_lock);
    if (have) {
        advertise(&f);
        s_frame_until_ms = now_ms + FRAME_WINDOW_MS;
    } else if (s_advertising) {
        // Nothing new: stop rather than repeat a stale frame (a repeated ACCEPT would re-trigger).
        ble_gap_ext_adv_stop(MESH_ADV_INSTANCE);
        s_advertising = false;
    }
}

const mesh_transport_t mesh_transport_ble = {
    .name = "ble",
    .start = ble_start,
    .send = ble_send,
    .poll = ble_poll,
};
