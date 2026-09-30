#include "ancs_bridge.h"

#include "board_profile.h"

#if BOARD_HAS_ANCS

#include "ancs_core.h"
#include "ancs_nav.h"
#include "apple_client.h"
#include "call_packet.h"
#include "packet.h"
#include "pipeline.h"

#include "esp_log.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"

#include <string.h>

static const char *TAG = "ancs_bridge";

// Threading: everything below is owned by the NimBLE host task. The apple_client callbacks already
// run there, and ancs_bridge_tick / ancs_bridge_accept_call (called from countdown_task) only post
// an event onto NimBLE's default event queue, so the work itself runs on the host task too. The
// only state countdown_task touches directly is the volatile call id it checks a tap against and
// the pending CTS time, which is guarded by a critical section.

static QueueHandle_t s_raw_queue;
static QueueHandle_t s_call_queue;

// Our own sequence counter: ANCS has no sequence of its own, and the countdown's re-baselining
// wants a monotonic one.
static uint32_t s_sequence;

// The one incoming call ANCS has told us about. Only one ever matters -- the terminal shows one
// call page -- so this is a scalar rather than a table.
//
// call_state's ids are u16 and the ANCS UID is u32, so the low 16 bits become the call_id and the
// full UID is kept here for the Perform Notification Action that answers it. A truncated id is
// safe for what it is used for: matching a tap to the call currently on screen.
static bool s_call_live;
static uint32_t s_call_uid;
static uint16_t s_call_id;
static call_event_t s_call_event;
static uint32_t s_call_refresh_at_ms;

// What countdown_task may read: the live call's id, or -1 for none. Written by the host task
// whenever s_call_live / s_call_id change; a single aligned word, so no lock.
static volatile int32_t s_live_call_id = -1;
// The tap countdown_task has handed over, consumed by the accept event on the host task.
static volatile uint16_t s_accept_call_id;

// CTS time arrives on the host task but the wall clock belongs to the pipeline, which only
// countdown_task may touch (SPEC 8.1). Parked here and applied on the next ancs_bridge_tick.
static portMUX_TYPE s_time_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_time_pending;
static uint32_t s_time_epoch_s;
static uint32_t s_time_at_ms;

// Host-task events. Initialised lazily from countdown_task's first tick: the NimBLE event pool
// only exists after nimble_port_init(), which runs after ancs_bridge_init().
static struct ble_npl_event s_tick_event;
static struct ble_npl_event s_accept_event;
static bool s_events_ready;

// call_state drops a ring not refreshed within CALL_RING_TIMEOUT_MS. The Android relay re-posts
// every 5 s; ANCS says nothing after the initial announcement, so the refresh is ours to do.
#define ANCS_CALL_REFRESH_MS 5000u

void ancs_bridge_init(QueueHandle_t raw_packet_queue, QueueHandle_t call_event_queue) {
    s_raw_queue = raw_packet_queue;
    s_call_queue = call_event_queue;
    s_sequence = 0;
    s_call_live = false;
    s_live_call_id = -1;
}

// Both of these run on the NimBLE host task, so they only ever enqueue -- never render, never
// block. Same rule as ble_link.c's write path (SPEC 8.1).
static void send_call(const call_event_t *ev) {
    if (s_call_queue != NULL) {
        (void)xQueueSend(s_call_queue, ev, pdMS_TO_TICKS(10));
    }
}

static void handle_call(const ancs_notification_t *n, bool removed) {
    if (removed) {
        if (!s_call_live || n->uid != s_call_uid) {
            return;
        }
        s_call_live = false;
        s_live_call_id = -1;
        call_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = CALL_EVENT_CLEAR;
        ev.call_id = s_call_id;
        send_call(&ev);
        return;
    }

    s_call_live = true;
    s_call_uid = n->uid;
    s_call_id = (uint16_t)(n->uid & 0xFFFFu);
    s_live_call_id = s_call_id;

    memset(&s_call_event, 0, sizeof(s_call_event));
    s_call_event.type = CALL_EVENT_RING;
    s_call_event.call_id = s_call_id;
    // ANCS gives one Title, which is the caller's name when iOS has a contact for them and the
    // number when it does not. There is no separate number attribute, so the number line stays
    // empty rather than guessing -- CallScreen already handles that.
    call_fold_ascii((const uint8_t *)n->title, strlen(n->title), s_call_event.caller,
                    CALL_CALLER_LEN);
    send_call(&s_call_event);
    s_call_refresh_at_ms = platform_now_ms() + ANCS_CALL_REFRESH_MS;
}

static void handle_nav(const ancs_notification_t *n) {
    static raw_notif_t raw; // 210 bytes-ish; too much for the host task's stack
    if (!ancs_nav_to_raw(n, ++s_sequence, &raw)) {
        return;
    }
    if (s_raw_queue != NULL) {
        (void)xQueueSend(s_raw_queue, &raw, pdMS_TO_TICKS(10));
    }
}

void ancs_bridge_on_notification(const ancs_notification_t *n, bool removed) {
    if (n == NULL) {
        return;
    }

    if (n->category == ANCS_CATEGORY_INCOMING_CALL) {
        handle_call(n, removed);
        return;
    }
    // A removal that is not the call is nothing to do: navigation ending is announced by Maps
    // withdrawing its notification, and the nav page already ages out on its own staleness rule
    // rather than needing a positive "stop" it might never get.
    if (removed) {
        return;
    }
    if (!ancs_nav_is_navigation(n)) {
        return;
    }
    handle_nav(n);
}

void ancs_bridge_on_time(uint32_t local_epoch_s) {
    const uint32_t now_ms = platform_now_ms();
    portENTER_CRITICAL(&s_time_mux);
    s_time_epoch_s = local_epoch_s;
    s_time_at_ms = now_ms;
    s_time_pending = true;
    portEXIT_CRITICAL(&s_time_mux);
}

// Host task: discovery retries and the ANCS ring refresh.
static void on_tick_event(struct ble_npl_event *ev) {
    (void)ev;
    const uint32_t now_ms = platform_now_ms();
    apple_client_tick(now_ms);
    if (!s_call_live || (int32_t)(now_ms - s_call_refresh_at_ms) < 0) {
        return;
    }
    s_call_refresh_at_ms = now_ms + ANCS_CALL_REFRESH_MS;
    send_call(&s_call_event);
}

// Host task: answers the call countdown_task claimed, if it is still the one ringing.
static void on_accept_event(struct ble_npl_event *ev) {
    (void)ev;
    if (!s_call_live || s_accept_call_id != s_call_id) {
        return;
    }
    // ANCS has no "answer" command as such: answering is the notification's POSITIVE action,
    // which for CategoryIDIncomingCall is what iOS maps to picking up (SPEC 5.5).
    if (!apple_client_perform_action(s_call_uid, ANCS_ACTION_POSITIVE)) {
        ESP_LOGW(TAG, "could not answer uid=%u", (unsigned)s_call_uid);
    }
}

static void ensure_events(void) {
    if (s_events_ready) {
        return;
    }
    ble_npl_event_init(&s_tick_event, on_tick_event, NULL);
    ble_npl_event_init(&s_accept_event, on_accept_event, NULL);
    s_events_ready = true;
}

void ancs_bridge_tick(uint32_t now_ms) {
    (void)now_ms;
    bool have_time;
    uint32_t epoch_s = 0, at_ms = 0;
    portENTER_CRITICAL(&s_time_mux);
    have_time = s_time_pending;
    if (have_time) {
        epoch_s = s_time_epoch_s;
        at_ms = s_time_at_ms;
        s_time_pending = false;
    }
    portEXIT_CRITICAL(&s_time_mux);
    if (have_time) {
        pipeline_set_wall_clock(epoch_s, at_ms);
    }

    ensure_events();
    // A no-op while the previous tick is still queued, so a busy host task never piles them up.
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_tick_event);
}

bool ancs_bridge_accept_call(uint16_t call_id) {
    if (s_live_call_id != (int32_t)call_id) {
        return false;
    }
    ensure_events();
    s_accept_call_id = call_id;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_accept_event);
    // Claim the tap either way: the call is an ANCS one, so falling through to the Android
    // CALL_ACTION notify would write a call id the phone has never heard of.
    return true;
}

#else

void ancs_bridge_init(QueueHandle_t raw_packet_queue, QueueHandle_t call_event_queue) {
    (void)raw_packet_queue;
    (void)call_event_queue;
}
void ancs_bridge_on_notification(const ancs_notification_t *n, bool removed) {
    (void)n;
    (void)removed;
}
void ancs_bridge_on_time(uint32_t local_epoch_s) { (void)local_epoch_s; }
void ancs_bridge_tick(uint32_t now_ms) { (void)now_ms; }
// Never ours, so the tap falls through to the Android CALL_ACTION notify.
bool ancs_bridge_accept_call(uint16_t call_id) {
    (void)call_id;
    return false;
}

#endif
