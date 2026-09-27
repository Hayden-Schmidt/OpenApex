#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Incoming-call relay (phone <-> terminal). Two small packets on their own characteristics,
// separate from the 200-byte nav packet so a ring is never held behind the ~3 Hz nav cadence and
// the nav packet needs no version bump. See the Android CallPacket.kt KDoc for the mirror.
//
// CALL_EVENT, phone -> terminal (written with the same 3-byte fragment header as nav packets):
//   [0]     version (CALL_PACKET_VERSION)
//   [1]     type: CALL_EVENT_RING / CALL_EVENT_ACTIVE / CALL_EVENT_CLEAR
//   [2..3]  call_id, u16 LE -- phone-assigned, echoed back in the accept so a late tap can never
//           answer a different call
//   [4..43] caller, UTF-8, NUL-padded: the contact name, or the number when there is no contact.
//           Whatever the phone's dialer shows; the terminal never looks anything up. Empty when
//           the dialer only knows the number (the terminal shows "Unknown Caller").
//   [44..63] number, UTF-8, NUL-padded; empty when withheld or not recoverable.
//
// CALL_ACTION, terminal -> phone (one notification, fits the default MTU unfragmented):
//   [0]     version (CALL_PACKET_VERSION)
//   [1]     action: CALL_ACTION_ACCEPT
//   [2..3]  call_id, u16 LE
//
// There is no reject: the rider lets the call ring out, and the phone's CLEAR takes the screen down.
#define CALL_PACKET_VERSION 1U
#define CALL_EVENT_PACKET_SIZE 64U
#define CALL_ACTION_PACKET_SIZE 4U
#define CALL_CALLER_WIRE_LEN 40U
#define CALL_NUMBER_WIRE_LEN 20U
// Display copies: wire bytes after folding to the font's ASCII set, plus terminator.
#define CALL_CALLER_LEN (CALL_CALLER_WIRE_LEN + 1U)
#define CALL_NUMBER_LEN (CALL_NUMBER_WIRE_LEN + 1U)

typedef enum {
    CALL_EVENT_RING = 1,  // ringing (re-sent periodically while it still is -- see call_state.h)
    CALL_EVENT_CLEAR = 2, // over: rejected on the phone, rang out or hung up
    CALL_EVENT_ACTIVE = 3, // answered (here or on the phone); re-sent periodically like RING
} call_event_type_t;

typedef enum {
    CALL_ACTION_ACCEPT = 1,
} call_action_t;

typedef struct {
    call_event_type_t type;
    uint16_t call_id;
    char caller[CALL_CALLER_LEN];
    char number[CALL_NUMBER_LEN];
} call_event_t;

// Decodes a CALL_EVENT packet. Returns false on wrong version, unknown type or short length. The
// caller and number strings are folded to printable ASCII (the only glyphs the terminal's fonts carry): common
// Latin accents lose their mark, anything else becomes '?'.
bool call_event_decode(const uint8_t *data, size_t len, call_event_t *out);

// Encodes a CALL_ACTION packet into out (CALL_ACTION_PACKET_SIZE bytes).
void call_action_encode(call_action_t action, uint16_t call_id, uint8_t out[CALL_ACTION_PACKET_SIZE]);
