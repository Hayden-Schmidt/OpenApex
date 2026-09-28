#include "ancs_bridge.h"

#include "ancs_core.h"
#include "ancs_nav.h"
#include "apple_client.h"
#include "call_packet.h"
#include "packet.h"
#include "pipeline.h"

#include "esp_log.h"

#include <string.h>

static const char *TAG = "ancs_bridge";

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

// call_state drops a ring not refreshed within CALL_RING_TIMEOUT_MS. The Android relay re-posts
// every 5 s; ANCS says nothing after the initial announcement, so the refresh is ours to do.
#define ANCS_CALL_REFRESH_MS 5000u

void ancs_bridge_init(QueueHandle_t raw_packet_queue, QueueHandle_t call_event_queue) {
    s_raw_queue = raw_packet_queue;
    s_call_queue = call_event_queue;
    s_sequence = 0;
    s_call_live = false;
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

    memset(&s_call_event, 0, sizeof(s_call_event));
    s_call_event.type = CALL_EVENT_RING;
    s_call_event.call_id = s_call_id;
    // ANCS gives one Title, which is the caller's name when iOS has a contact for them and the
    // number when it does not. There is no separate number attribute, so the number line stays
    // empty rather than guessing -- CallScreen already handles that.
    call_fold_ascii((const uint8_t *)n->title, strlen(n->title), s_call_event.caller,
                    CALL_CALLER_LEN);
    send_call(&s_call_event);
    s_call_refresh_at_ms = 0; // set on the next tick, which owns the clock
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
    pipeline_set_wall_clock(local_epoch_s, platform_now_ms());
}

void ancs_bridge_tick(uint32_t now_ms) {
    apple_client_tick(now_ms);
    if (!s_call_live) {
        return;
    }
    if (s_call_refresh_at_ms == 0) {
        s_call_refresh_at_ms = now_ms + ANCS_CALL_REFRESH_MS;
        return;
    }
    if ((int32_t)(now_ms - s_call_refresh_at_ms) < 0) {
        return;
    }
    s_call_refresh_at_ms = now_ms + ANCS_CALL_REFRESH_MS;
    send_call(&s_call_event);
}

bool ancs_bridge_accept_call(uint16_t call_id) {
    if (!s_call_live || call_id != s_call_id) {
        return false;
    }
    // ANCS has no "answer" command as such: answering is the notification's POSITIVE action,
    // which for CategoryIDIncomingCall is what iOS maps to picking up (SPEC 5.5).
    const bool ok = apple_client_perform_action(s_call_uid, ANCS_ACTION_POSITIVE);
    if (!ok) {
        ESP_LOGW(TAG, "could not answer uid=%u", (unsigned)s_call_uid);
    }
    // Claim the tap either way: the call is an ANCS one, so falling through to the Android
    // CALL_ACTION notify would write a call id the phone has never heard of.
    return true;
}
