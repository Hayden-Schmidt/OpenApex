#include "call_state.h"

#include <string.h>

static bool s_ringing;
static uint16_t s_call_id;
static char s_caller[CALL_CALLER_LEN];
static uint32_t s_last_ring_ms;

void call_state_reset(void) {
    s_ringing = false;
    s_call_id = 0;
    s_caller[0] = '\0';
    s_last_ring_ms = 0;
}

void call_state_apply(const call_event_t *event, uint32_t now_ms) {
    if (event->type == CALL_EVENT_RING) {
        s_ringing = true;
        s_call_id = event->call_id;
        memcpy(s_caller, event->caller, sizeof(s_caller));
        s_caller[sizeof(s_caller) - 1] = '\0';
        s_last_ring_ms = now_ms;
        return;
    }
    // A CLEAR for some other call (a second call waiting that ended) leaves this one up.
    if (s_ringing && event->call_id == s_call_id) {
        s_ringing = false;
    }
}

void call_state_tick(uint32_t now_ms, bool link_connected) {
    if (!s_ringing) {
        return;
    }
    if (!link_connected || (uint32_t)(now_ms - s_last_ring_ms) > CALL_RING_TIMEOUT_MS) {
        s_ringing = false;
    }
}

void call_state_fill(terminal_view_state_t *out) {
    out->call_ringing = s_ringing;
    out->call_id = s_call_id;
    memcpy(out->caller, s_caller, sizeof(out->caller));
}
