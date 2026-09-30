#include "mesh.h"

#include "ble_link.h"
#include "settings.h"

#include "board_profile.h"
#include "mesh_transport.h"

#include "esp_log.h"
#include "esp_mac.h"
#include <stdio.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "mesh";

// Same namespace as settings_store.c and avatar_store.c: identity and friends are rider data too.
#define MESH_NAMESPACE "settings"
#define IDENT_KEY "ident"
#define FRIENDS_KEY "friends"

#define RX_QUEUE_LEN 8
#define ACCEPT_QUEUE_LEN 2
#define REQUEST_QUEUE_LEN 2
#define TASK_PERIOD_MS 50
#define DEDUP_LEN 16 // BLE repeats each advert a few times, and dual-radio boards hear frames twice

// Friends characteristic ops (docs/reference/rider_mesh_v1.md).
enum { OP_UPSERT = 1, OP_REMOVE = 2, OP_CLEAR = 3, OP_CURSOR = 4 };

typedef struct {
    uint8_t len;
    uint8_t data[MESH_FRAME_MAX];
} rx_frame_t;

// BLE first: the baseline every terminal speaks (mesh_transport.h).
static const mesh_transport_t *const kTransports[] = {
    &mesh_transport_ble,
#if BOARD_MESH_ESPNOW
    &mesh_transport_espnow,
#endif
};
#define TRANSPORT_COUNT (sizeof(kTransports) / sizeof(kTransports[0]))
static bool s_transport_up[TRANSPORT_COUNT];

static mesh_core_t s_core;
static SemaphoreHandle_t s_mutex;
static QueueHandle_t s_rx_queue;
static QueueHandle_t s_accept_queue;
static QueueHandle_t s_request_queue;
static volatile bool s_prompt_allowed;
static volatile bool s_menu_open;
static uint8_t s_friend_cursor; // NimBLE host task only
static volatile bool s_ready; // a transport is up; set once by mesh_task

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void lock(void) { xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mutex); }

// -- NVS -----------------------------------------------------------------------------------------

static bool nvs_put(const char *key, const void *buf, size_t len) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(MESH_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = len == 0 ? nvs_erase_key(handle, key) : nvs_set_blob(handle, key, buf, len);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK) ESP_LOGW(TAG, "nvs write %s failed (%s)", key, esp_err_to_name(err));
    return err == ESP_OK;
}

static size_t nvs_get(const char *key, void *buf, size_t cap) {
    nvs_handle_t handle;
    if (nvs_open(MESH_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return 0;
    size_t len = cap;
    const esp_err_t err = nvs_get_blob(handle, key, buf, &len);
    nvs_close(handle);
    return err == ESP_OK ? len : 0;
}

// Scratch for the whole friend list as NVS records. Shared by load (mesh_init, before any task runs)
// and save (mesh task, mutex held), which never overlap.
static uint8_t s_list_buf[MESH_MAX_FRIENDS * MESH_FRIEND_RECORD_LEN];

static void load_stored(void) {
    uint8_t buf[MESH_IDENTITY_MAX_LEN];
    mesh_identity_t id;
    const size_t len = nvs_get(IDENT_KEY, buf, sizeof(buf));
    if (len > 0 && mesh_identity_decode(buf, len, &id)) mesh_core_set_identity(&s_core, &id);

    const size_t n = nvs_get(FRIENDS_KEY, s_list_buf, sizeof(s_list_buf)) / MESH_FRIEND_RECORD_LEN;
    for (size_t i = 0; i < n; i++) {
        mesh_friend_t f;
        if (mesh_friend_decode(&s_list_buf[i * MESH_FRIEND_RECORD_LEN], &f)) mesh_core_friend_upsert(&s_core, &f);
    }
    ESP_LOGI(TAG, "loaded identity=%d friends=%u", s_core.has_identity, (unsigned)s_core.friend_count);
}

// Called with the mutex held; the list is at most ~2 KB, so one blob write.
static void save_friends(void) {
    for (size_t i = 0; i < s_core.friend_count; i++) {
        mesh_friend_encode(&s_core.friends[i], &s_list_buf[i * MESH_FRIEND_RECORD_LEN]);
    }
    nvs_put(FRIENDS_KEY, s_list_buf, s_core.friend_count * MESH_FRIEND_RECORD_LEN);
}

// -- radio ---------------------------------------------------------------------------------------

static void send_frame(const uint8_t *frame, size_t len, void *ctx) {
    (void)ctx;
#ifdef MESH_BENCH_TEST
    ESP_LOGI(TAG, "tx type %u seq %u len %u", frame[3], frame[12] | frame[13] << 8, (unsigned)len);
#endif
    for (size_t i = 0; i < TRANSPORT_COUNT; i++) {
        if (s_transport_up[i]) kTransports[i]->send(frame, len);
    }
}

// Transport context: copy out and return.
static void on_recv(const uint8_t *data, size_t len) {
    if (len == 0 || len > MESH_FRAME_MAX) return;
    rx_frame_t f;
    f.len = (uint8_t)len;
    memcpy(f.data, data, len);
    xQueueSend(s_rx_queue, &f, 0);
}

// True the first time a (sender, type, seq) is seen among the last DEDUP_LEN frames. mesh task only.
static bool first_sighting(const uint8_t *frame, size_t len) {
    typedef struct {
        uint8_t sender[MESH_RIDER_ID_LEN];
        uint8_t type;
        uint16_t seq;
    } seen_t;
    static seen_t seen[DEDUP_LEN];
    static size_t next;
    mesh_header_t h;
    if (!mesh_decode_header(frame, len, &h)) return true; // let mesh_core reject it
    for (size_t i = 0; i < DEDUP_LEN; i++) {
        if (seen[i].type == h.type && seen[i].seq == h.seq &&
            memcmp(seen[i].sender, h.sender, sizeof(h.sender)) == 0) {
            return false;
        }
    }
    memcpy(seen[next].sender, h.sender, sizeof(h.sender));
    seen[next].type = h.type;
    seen[next].seq = h.seq;
    next = (next + 1u) % DEDUP_LEN;
    return true;
}

// -- own look ------------------------------------------------------------------------------------
// Avatar number and accent only: custom art stays on this terminal (POC, blueprint D1).

static void refresh_look(void) {
    const terminal_settings_t *s = settings_get();
    mesh_look_t look;
    memset(&look, 0, sizeof(look));
    look.avatar = s->avatar;
    look.accent_rgb = s->accent_rgb;
    lock();
    mesh_core_set_look(&s_core, &look);
    mesh_core_set_enabled(&s_core, s->mesh_enabled != 0);
    unlock();
}

// -- task ----------------------------------------------------------------------------------------

static void notify_friend_events(void) {
    mesh_friend_event_t ev;
    for (;;) {
        lock();
        const bool got = mesh_core_take_friend_event(&s_core, &ev);
        unlock();
        if (!got) return;
        ESP_LOGI(TAG, "friend %s: '%s'", ev.kind == MESH_FRIEND_EVENT_ADDED ? "added" : "updated", ev.record.look.name);
        uint8_t buf[1 + MESH_FRIEND_RECORD_LEN];
        buf[0] = OP_UPSERT;
        mesh_friend_encode(&ev.record, &buf[1]);
        // Not connected: the phone reads and merges the terminal's list on its next connect.
        ble_link_send_friend(buf, sizeof(buf));
    }
}

static void mesh_task(void *arg) {
    (void)arg;
    bool any = false;
    for (size_t i = 0; i < TRANSPORT_COUNT; i++) {
        s_transport_up[i] = kTransports[i]->start(on_recv);
        ESP_LOGI(TAG, "transport %s %s", kTransports[i]->name, s_transport_up[i] ? "up" : "failed");
        any |= s_transport_up[i];
    }
    if (!any) vTaskDelete(NULL);
    s_ready = true;
    uint32_t look_gen = settings_generation() - 1u;
    uint32_t saved_gen = s_core.friends_generation;
    bool menu_open = false;
#if defined(OPENAPEX_MESH_CONSOLE) && OPENAPEX_MESH_CONSOLE
    uint32_t stack_log_at = 0;
#endif
    rx_frame_t f;
    for (;;) {
        const bool got = xQueueReceive(s_rx_queue, &f, pdMS_TO_TICKS(TASK_PERIOD_MS)) == pdTRUE;
        const uint32_t now = now_ms();
        if (settings_generation() != look_gen) {
            look_gen = settings_generation();
            refresh_look();
        }
        lock();
        if (got) {
            do {
#ifdef MESH_BENCH_TEST
                ESP_LOGI(TAG, "rx type %u seq %u len %u%s", f.data[3], f.data[12] | f.data[13] << 8, f.len,
                         first_sighting(f.data, f.len) ? "" : " dup");
                if (1) mesh_core_rx(&s_core, f.data, f.len, now, send_frame, NULL); else
#endif
                if (first_sighting(f.data, f.len)) mesh_core_rx(&s_core, f.data, f.len, now, send_frame, NULL);
            } while (xQueueReceive(s_rx_queue, &f, 0) == pdTRUE);
        }
        uint8_t id[MESH_RIDER_ID_LEN];
        if (s_menu_open != menu_open) {
            menu_open = s_menu_open;
            mesh_core_set_menu_open(&s_core, menu_open, now);
        }
        while (xQueueReceive(s_request_queue, id, 0) == pdTRUE) {
            if (!mesh_core_request(&s_core, id, now, send_frame, NULL)) ESP_LOGW(TAG, "request refused");
        }
        while (xQueueReceive(s_accept_queue, id, 0) == pdTRUE) {
            mesh_core_accept(&s_core, id, now, send_frame, NULL);
        }
        mesh_core_tick(&s_core, now, s_prompt_allowed, send_frame, NULL);
#ifdef MESH_BENCH_TEST
        {
            static mesh_prompt_state_t last;
            mesh_look_t pl;
            uint8_t pid[MESH_RIDER_ID_LEN];
            const mesh_prompt_state_t st = mesh_core_prompt(&s_core, pid, &pl);
            if (st != last) ESP_LOGI(TAG, "prompt %d peer '%s'", st, pl.name);
            if (st == MESH_PROMPT_OFFER) mesh_core_accept(&s_core, pid, now, send_frame, NULL);
            last = st;
            static uint32_t pos_at;
            if (now - pos_at > 1000) {
                pos_at = now;
                const mesh_pos_t p = {.lat_e7 = -338688000 + (int32_t)(s_core.self.id[5]) * 100,
                                      .lon_e7 = 1512093000, .speed_kmh_x10 = 0xFFFF, .heading_deg_x10 = 0xFFFF};
                mesh_core_set_own_pos(&s_core, &p, now);
                mesh_friend_fix_t fx[4];
                const size_t n = mesh_core_friends_in_range(&s_core, now, fx, 4);
                for (size_t i = 0; i < n; i++)
                    ESP_LOGI(TAG, "friend '%s' at %.0f deg %.1f m", fx[i].f->look.name, fx[i].bearing_deg, fx[i].dist_m);
            }
        }
#endif
        if (s_core.friends_generation != saved_gen) {
            saved_gen = s_core.friends_generation;
            save_friends();
        }
        unlock();
        notify_friend_events();
#if defined(OPENAPEX_MESH_CONSOLE) && OPENAPEX_MESH_CONSOLE
        if (now - stack_log_at >= 10000u) {
            stack_log_at = now;
            ESP_LOGI(TAG, "mesh_task stack high-water %u bytes free", (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
#endif
        for (size_t i = 0; i < TRANSPORT_COUNT; i++) {
            if (s_transport_up[i] && kTransports[i]->poll != NULL) kTransports[i]->poll(now);
        }
    }
}

void mesh_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    s_rx_queue = xQueueCreate(RX_QUEUE_LEN, sizeof(rx_frame_t));
    s_accept_queue = xQueueCreate(ACCEPT_QUEUE_LEN, MESH_RIDER_ID_LEN);
    s_request_queue = xQueueCreate(REQUEST_QUEUE_LEN, MESH_RIDER_ID_LEN);
    mesh_core_init(&s_core, esp_random());
    load_stored();
#ifdef MESH_BENCH_TEST
    mesh_core_friend_clear(&s_core); // re-friend from scratch every bench boot
    if (!s_core.has_identity) {
        mesh_identity_t id = {0};
        esp_read_mac(id.id, ESP_MAC_WIFI_STA);
        for (int i = 0; i < 16; i++) id.key[i] = (uint8_t)(id.id[i % 8] ^ (i * 37)); // stable across boots
        snprintf(id.name, sizeof(id.name), "C3-%02X", id.id[5]);
        mesh_core_set_identity(&s_core, &id);
    }
#endif
}

void mesh_start(void) { xTaskCreate(mesh_task, "mesh_task", 4096, NULL, 3, NULL); }

// -- public --------------------------------------------------------------------------------------

static volatile bool s_pos_locked; // development console: a simulated position replaces the phone's

void mesh_debug_set_pos(const mesh_pos_t *pos, bool lock_out_phone) {
    s_pos_locked = lock_out_phone;
    if (!s_ready || pos == NULL) return;
    lock();
    mesh_core_set_own_pos(&s_core, pos, now_ms());
    unlock();
}

void mesh_set_own_pos(const mesh_pos_t *pos) {
    if (!s_ready || s_pos_locked) return;
    const uint32_t now = now_ms();
    lock();
    mesh_core_set_own_pos(&s_core, pos, now);
    unlock();
}

void mesh_set_prompt_allowed(bool allowed) { s_prompt_allowed = allowed; }

void mesh_request_accept(const uint8_t id[MESH_RIDER_ID_LEN]) {
    if (s_ready) xQueueSend(s_accept_queue, id, 0);
}

void mesh_set_menu_open(bool open) { s_menu_open = open; }

void mesh_request_friend(const uint8_t id[MESH_RIDER_ID_LEN]) {
    if (s_ready) xQueueSend(s_request_queue, id, 0);
}

size_t mesh_get_nearby(mesh_nearby_t *out, size_t cap) {
    if (!s_ready) return 0;
    const mesh_peer_t *peers[MESH_MAX_PEERS];
    if (cap > MESH_MAX_PEERS) cap = MESH_MAX_PEERS;
    lock();
    const size_t n = mesh_core_nearby(&s_core, now_ms(), peers, cap);
    for (size_t i = 0; i < n; i++) {
        memcpy(out[i].id, peers[i]->id, MESH_RIDER_ID_LEN);
        out[i].look = peers[i]->look;
    }
    unlock();
    return n;
}

bool mesh_debug_make_identity(void) {
    mesh_identity_t id;
    memset(&id, 0, sizeof(id));
    lock();
    const bool have = s_core.has_identity;
    unlock();
    if (have) return false;
    esp_read_mac(id.id, ESP_MAC_WIFI_STA);
    for (int i = 0; i < 16; i++) id.key[i] = (uint8_t)(id.id[i % 8] ^ (i * 37));
    snprintf(id.name, sizeof(id.name), "C3-%02X", id.id[5]);
    uint8_t buf[MESH_IDENTITY_MAX_LEN];
    const size_t n = mesh_identity_encode(&id, buf, sizeof(buf));
    lock();
    mesh_core_set_identity(&s_core, &id);
    unlock();
    return n > 0 && nvs_put(IDENT_KEY, buf, n);
}

void mesh_forget_all_friends(void) {
    lock();
    mesh_core_friend_clear(&s_core);
    unlock();
}

mesh_prompt_state_t mesh_get_prompt(uint8_t id[MESH_RIDER_ID_LEN], mesh_look_t *peer, mesh_look_t *self) {
    if (!s_ready) return MESH_PROMPT_NONE;
    lock();
    const mesh_prompt_state_t st = mesh_core_prompt(&s_core, id, peer);
    if (st != MESH_PROMPT_NONE) {
        *self = s_core.look;
        memcpy(self->name, s_core.self.name, sizeof(self->name));
    }
    unlock();
    return st;
}

size_t mesh_get_friends_in_range(mesh_friend_marker_t *out, size_t cap) {
    if (!s_ready) return 0;
    mesh_friend_fix_t fix[MESH_MAX_PEERS];
    if (cap > MESH_MAX_PEERS) cap = MESH_MAX_PEERS;
    lock();
    const size_t n = mesh_core_friends_in_range(&s_core, now_ms(), fix, cap);
    for (size_t i = 0; i < n; i++) {
        out[i].look = fix[i].f->look;
        out[i].bearing_deg = fix[i].bearing_deg;
        out[i].dist_m = fix[i].dist_m;
    }
    unlock();
    return n;
}

// -- BLE (NimBLE host task) ----------------------------------------------------------------------

static int read_flat(struct ble_gatt_access_ctxt *ctxt, uint8_t *buf, uint16_t cap, uint16_t *len) {
    if (OS_MBUF_PKTLEN(ctxt->om) > cap) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    return ble_hs_mbuf_to_flat(ctxt->om, buf, cap, len) == 0 ? 0 : BLE_ATT_ERR_UNLIKELY;
}

int mesh_identity_ble_access(struct ble_gatt_access_ctxt *ctxt) {
    uint8_t buf[MESH_IDENTITY_MAX_LEN];
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        lock();
        const size_t len = s_core.has_identity ? mesh_identity_encode(&s_core.self, buf, sizeof(buf)) : 0;
        unlock();
        return os_mbuf_append(ctxt->om, buf, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    uint16_t len = 0;
    int rc = read_flat(ctxt, buf, sizeof(buf), &len);
    if (rc != 0) return rc;
    mesh_identity_t id;
    if (!mesh_identity_decode(buf, len, &id)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    lock();
    const bool same = s_core.has_identity && memcmp(&s_core.self, &id, sizeof(id)) == 0;
    mesh_core_set_identity(&s_core, &id);
    unlock();
    // The phone writes it on every connect; only a change costs a flash write.
    if (!same && !nvs_put(IDENT_KEY, buf, len)) return BLE_ATT_ERR_UNLIKELY;
    if (!same) ESP_LOGI(TAG, "identity set, name '%s'", id.name);
    return 0;
}

int mesh_friends_ble_access(struct ble_gatt_access_ctxt *ctxt) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        // The record at the cursor; empty past the end, which ends the phone's read loop.
        uint8_t rec[MESH_FRIEND_RECORD_LEN];
        lock();
        const bool have = s_friend_cursor < s_core.friend_count;
        if (have) mesh_friend_encode(&s_core.friends[s_friend_cursor], rec);
        unlock();
        return os_mbuf_append(ctxt->om, rec, have ? sizeof(rec) : 0) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    uint8_t buf[1 + MESH_FRIEND_RECORD_LEN];
    uint16_t len = 0;
    int rc = read_flat(ctxt, buf, sizeof(buf), &len);
    if (rc != 0) return rc;
    if (len < 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    mesh_friend_t f;
    rc = 0;
    lock();
    switch (buf[0]) {
    case OP_UPSERT:
        if (len != sizeof(buf) || !mesh_friend_decode(&buf[1], &f)) rc = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        else if (!mesh_core_friend_upsert(&s_core, &f)) rc = BLE_ATT_ERR_INSUFFICIENT_RES;
        break;
    case OP_REMOVE:
        if (len != 1 + MESH_RIDER_ID_LEN) rc = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        else mesh_core_friend_remove(&s_core, &buf[1]);
        break;
    case OP_CLEAR:
        mesh_core_friend_clear(&s_core);
        break;
    case OP_CURSOR:
        if (len != 2) rc = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        else s_friend_cursor = buf[1];
        break;
    default:
        rc = BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }
    unlock();
    // The mesh task sees friends_generation move and persists the list.
    return rc;
}
