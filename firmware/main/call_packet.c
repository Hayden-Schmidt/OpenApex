#include "call_packet.h"

#include <string.h>

// Base letter for U+00C0..U+00FF (Latin-1 Supplement letters), indexed from U+00C0. '?' where the
// character has no sensible ASCII stand-in. Sized for the terminator the literal carries.
static const char kLatin1Fold[65] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTs"
                                    "aaaaaaaceeeeiiiidnooooo/ouuuuyty";

// Folds UTF-8 src (bounded by src_len, stops at NUL) into printable ASCII. Multi-byte sequences
// outside Latin-1 collapse to a single '?', so a name in another script still shows its length
// rather than vanishing.
static void fold_ascii(const uint8_t *src, size_t src_len, char *dst, size_t dst_len) {
    size_t i = 0;
    size_t n = 0;
    while (i < src_len && src[i] != 0 && n < dst_len - 1) {
        const uint8_t c = src[i];
        if (c < 0x80) {
            dst[n++] = (c >= 0x20 && c != 0x7F) ? (char)c : ' ';
            i++;
            continue;
        }
        size_t seq = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        if (i + seq > src_len) {
            break; // truncated sequence at the end of the field
        }
        if (seq == 2 && c == 0xC3) {
            dst[n++] = kLatin1Fold[src[i + 1] & 0x3F];
        } else if (seq == 2 && c == 0xC2 && src[i + 1] == 0xA0) {
            dst[n++] = ' '; // no-break space
        } else {
            dst[n++] = '?';
        }
        i += seq;
    }
    dst[n] = '\0';
}

bool call_event_decode(const uint8_t *data, size_t len, call_event_t *out) {
    if (data == NULL || out == NULL || len < CALL_EVENT_PACKET_SIZE) {
        return false;
    }
    if (data[0] != CALL_PACKET_VERSION) {
        return false;
    }
    if (data[1] != CALL_EVENT_RING && data[1] != CALL_EVENT_CLEAR) {
        return false;
    }
    out->type = (call_event_type_t)data[1];
    out->call_id = (uint16_t)((uint16_t)data[2] | ((uint16_t)data[3] << 8));
    fold_ascii(&data[4], CALL_CALLER_WIRE_LEN, out->caller, CALL_CALLER_LEN);
    return true;
}

void call_action_encode(call_action_t action, uint16_t call_id, uint8_t out[CALL_ACTION_PACKET_SIZE]) {
    out[0] = CALL_PACKET_VERSION;
    out[1] = (uint8_t)action;
    out[2] = (uint8_t)(call_id & 0xFFu);
    out[3] = (uint8_t)(call_id >> 8);
}
