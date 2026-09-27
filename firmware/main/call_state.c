#include "call_state.h"

#include <string.h>

typedef enum { CALL_NONE, CALL_RINGING, CALL_ACTIVE } call_phase_t;

static call_phase_t s_phase;
static uint16_t s_call_id;
static char s_caller[CALL_CALLER_LEN];
static char s_number[CALL_NUMBER_LEN];
static uint32_t s_last_refresh_ms;
static uint32_t s_answered_ms;

void call_state_reset(void) {
    s_phase = CALL_NONE;
    s_call_id = 0;
    s_caller[0] = '\0';
    s_number[0] = '\0';
    s_last_refresh_ms = 0;
    s_answered_ms = 0;
}

void call_state_apply(const call_event_t *event, uint32_t now_ms) {
    if (event->type == CALL_EVENT_CLEAR) {
        // A CLEAR for some other call (a second call waiting that ended) leaves this one up.
        if (s_phase != CALL_NONE && event->call_id == s_call_id) {
            s_phase = CALL_NONE;
        }
        return;
    }
    const bool same_call = s_phase != CALL_NONE && event->call_id == s_call_id;
    if (event->type == CALL_EVENT_ACTIVE) {
        // Only a call first seen ringing gets a page: an outgoing call is not the terminal's business.
        if (!same_call) {
            return;
        }
        if (s_phase != CALL_ACTIVE) {
            s_phase = CALL_ACTIVE;
            s_answered_ms = now_ms;
        }
    } else {
        // A late RING refresh must not drop an answered call back to ringing.
        if (!same_call) {
            s_phase = CALL_RINGING;
        }
    }
    s_call_id = event->call_id;
    memcpy(s_caller, event->caller, sizeof(s_caller));
    s_caller[sizeof(s_caller) - 1] = '\0';
    memcpy(s_number, event->number, sizeof(s_number));
    s_number[sizeof(s_number) - 1] = '\0';
    s_last_refresh_ms = now_ms;
}

void call_state_tick(uint32_t now_ms, bool link_connected) {
    if (s_phase == CALL_NONE) {
        return;
    }
    if (!link_connected || (uint32_t)(now_ms - s_last_refresh_ms) > CALL_RING_TIMEOUT_MS) {
        s_phase = CALL_NONE;
    }
}

void call_state_fill(terminal_view_state_t *out, uint32_t now_ms) {
    out->call_ringing = s_phase == CALL_RINGING;
    out->call_active = s_phase == CALL_ACTIVE;
    out->call_elapsed_s = s_phase == CALL_ACTIVE ? (uint32_t)(now_ms - s_answered_ms) / 1000u : 0;
    out->call_id = s_call_id;
    memcpy(out->caller, s_caller, sizeof(out->caller));
    memcpy(out->call_number, s_number, sizeof(out->call_number));
}
