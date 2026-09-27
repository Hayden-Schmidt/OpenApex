#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../main/call_packet.h"
#include "../main/call_state.h"

static void make_event(uint8_t *buf, uint8_t version, uint8_t type, uint16_t id, const char *caller,
                       const char *number) {
    memset(buf, 0, CALL_EVENT_PACKET_SIZE);
    buf[0] = version;
    buf[1] = type;
    buf[2] = (uint8_t)(id & 0xFF);
    buf[3] = (uint8_t)(id >> 8);
    strncpy((char *)&buf[4], caller, CALL_CALLER_WIRE_LEN);
    strncpy((char *)&buf[4 + CALL_CALLER_WIRE_LEN], number, CALL_NUMBER_WIRE_LEN);
}

int main(void) {
    uint8_t buf[CALL_EVENT_PACKET_SIZE];
    call_event_t ev;

    // Golden bytes shared with the Android CallPacketTest: RING, id 0x1234, "Jane Doe", "+64 27 123".
    make_event(buf, 1, CALL_EVENT_RING, 0x1234, "Jane Doe", "+64 27 123");
    assert(buf[44] == '+');
    assert(buf[2] == 0x34 && buf[3] == 0x12);
    assert(call_event_decode(buf, sizeof(buf), &ev));
    assert(ev.type == CALL_EVENT_RING && ev.call_id == 0x1234);
    assert(strcmp(ev.caller, "Jane Doe") == 0);
    assert(strcmp(ev.number, "+64 27 123") == 0);
    make_event(buf, 1, CALL_EVENT_ACTIVE, 1, "x", "");
    assert(call_event_decode(buf, sizeof(buf), &ev) && ev.type == CALL_EVENT_ACTIVE);

    // Malformed packets never produce a call.
    assert(!call_event_decode(buf, sizeof(buf) - 1, &ev));
    make_event(buf, 2, CALL_EVENT_RING, 1, "x", "");
    assert(!call_event_decode(buf, sizeof(buf), &ev));
    make_event(buf, 1, 9, 1, "x", "");
    assert(!call_event_decode(buf, sizeof(buf), &ev));

    // UTF-8 folds to the fonts' ASCII: Latin accents lose their mark, other scripts become '?'.
    make_event(buf, 1, CALL_EVENT_RING, 1, "Zo\xC3\xAB M\xC3\xBCller \xE6\x9D\x8E", "");
    assert(call_event_decode(buf, sizeof(buf), &ev));
    assert(strcmp(ev.caller, "Zoe Muller ?") == 0);

    // A full-width caller is kept whole and terminated.
    char longname[CALL_CALLER_WIRE_LEN + 1];
    memset(longname, 'A', CALL_CALLER_WIRE_LEN);
    longname[CALL_CALLER_WIRE_LEN] = '\0';
    make_event(buf, 1, CALL_EVENT_RING, 1, longname, "");
    assert(call_event_decode(buf, sizeof(buf), &ev));
    assert(strlen(ev.caller) == CALL_CALLER_WIRE_LEN);

    uint8_t action[CALL_ACTION_PACKET_SIZE];
    call_action_encode(CALL_ACTION_ACCEPT, 0x1234, action);
    assert(action[0] == 1 && action[1] == 1 && action[2] == 0x34 && action[3] == 0x12);

    // State: ring -> clear.
    uint32_t now = 0;
    terminal_view_state_t view;
    memset(&view, 0, sizeof(view));
    call_state_reset();
    ev.type = CALL_EVENT_RING;
    ev.call_id = 7;
    strcpy(ev.caller, "+61 400 000 000");
    call_state_apply(&ev, 1000);
    call_state_tick(2000, true);
    call_state_fill(&view, now);
    assert(view.call_ringing && view.call_id == 7 && strcmp(view.caller, "+61 400 000 000") == 0);

    // A CLEAR for another call leaves this one up; its own CLEAR takes it down.
    ev.type = CALL_EVENT_CLEAR;
    ev.call_id = 8;
    call_state_apply(&ev, 2500);
    call_state_fill(&view, now);
    assert(view.call_ringing);
    ev.call_id = 7;
    call_state_apply(&ev, 3000);
    call_state_fill(&view, now);
    assert(!view.call_ringing);

    // Refreshed rings survive; an unrefreshed one expires; a link drop clears at once.
    ev.type = CALL_EVENT_RING;
    call_state_apply(&ev, 10000);
    call_state_apply(&ev, 10000 + CALL_RING_REFRESH_MS);
    call_state_tick(10000 + CALL_RING_REFRESH_MS + CALL_RING_TIMEOUT_MS - 1, true);
    call_state_fill(&view, now);
    assert(view.call_ringing);
    call_state_tick(10000 + CALL_RING_REFRESH_MS + CALL_RING_TIMEOUT_MS + 1, true);
    call_state_fill(&view, now);
    assert(!view.call_ringing);
    call_state_apply(&ev, 50000);
    call_state_tick(50001, false);
    call_state_fill(&view, now);
    assert(!view.call_ringing);

    // Ring -> answered -> clear. The duration runs from the first ACTIVE; a late RING refresh keeps
    // the call answered; ACTIVE for a call never seen ringing (outgoing) shows nothing.
    call_state_reset();
    ev.type = CALL_EVENT_ACTIVE;
    ev.call_id = 20;
    call_state_apply(&ev, 60000);
    call_state_fill(&view, 60000);
    assert(!view.call_active && !view.call_ringing);
    ev.type = CALL_EVENT_RING;
    call_state_apply(&ev, 60000);
    ev.type = CALL_EVENT_ACTIVE;
    call_state_apply(&ev, 61000);
    call_state_apply(&ev, 64000);
    ev.type = CALL_EVENT_RING;
    call_state_apply(&ev, 65000);
    call_state_tick(66000, true);
    call_state_fill(&view, 66500);
    assert(view.call_active && !view.call_ringing && view.call_elapsed_s == 5);
    ev.type = CALL_EVENT_CLEAR;
    call_state_apply(&ev, 67000);
    call_state_fill(&view, 67000);
    assert(!view.call_active && !view.call_ringing);

    puts("call tests passed");
    return 0;
}
