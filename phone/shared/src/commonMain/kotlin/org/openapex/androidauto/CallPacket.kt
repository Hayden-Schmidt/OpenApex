package org.openapex.androidauto


/**
 * Incoming-call relay packets. Mirror of firmware/main/call_packet.h -- keep the two byte-exact.
 *
 * CALL_EVENT, phone -> terminal (written to [RelayBleClient.CALL_EVENT_UUID], fragmented like nav
 * packets):
 *
 * | Offset | Size | Field   | Encoding                                              |
 * |--------|------|---------|-------------------------------------------------------|
 * | 0      | 1    | version | [CALL_PACKET_VERSION]                                 |
 * | 1      | 1    | type    | [CALL_EVENT_RING] / [CALL_EVENT_ACTIVE] / [CALL_EVENT_CLEAR] |
 * | 2      | 2    | call_id | uint16 LE, phone-assigned, echoed back in the accept  |
 * | 4      | 40   | caller  | UTF-8, NUL-padded, cut on a character boundary        |
 * | 44     | 20   | number  | UTF-8, NUL-padded; empty when withheld/unknown        |
 *
 * CALL_ACTION, terminal -> phone (notification on [RelayBleClient.CALL_ACTION_UUID]):
 * version (1), action (1, [CALL_ACTION_ACCEPT]), call_id (2, uint16 LE).
 *
 * The caller is the contact name the dialer resolved, empty when it only knows the number (the
 * terminal then shows "Unknown Caller"). The phone does no folding; the terminal folds to its font.
 */
const val CALL_PACKET_VERSION = 1
const val CALL_EVENT_PACKET_SIZE = 64
const val CALL_ACTION_PACKET_SIZE = 4
const val CALL_CALLER_WIRE_LEN = 40
const val CALL_NUMBER_WIRE_LEN = 20

const val CALL_EVENT_RING = 1
const val CALL_EVENT_CLEAR = 2
const val CALL_EVENT_ACTIVE = 3
const val CALL_ACTION_ACCEPT = 1

fun packCallEvent(type: Int, callId: Int, caller: String?, number: String? = null): ByteArray {
    val out = ByteArray(CALL_EVENT_PACKET_SIZE)
    out[0] = CALL_PACKET_VERSION.toByte()
    out[1] = type.toByte()
    out[2] = (callId and 0xFF).toByte()
    out[3] = ((callId shr 8) and 0xFF).toByte()
    val bytes = utf8Truncated(caller.orEmpty(), CALL_CALLER_WIRE_LEN)
    bytes.copyInto(out, 4)
    val num = utf8Truncated(number.orEmpty(), CALL_NUMBER_WIRE_LEN)
    num.copyInto(out, 4 + CALL_CALLER_WIRE_LEN)
    return out
}

/** Call id of a CALL_ACTION accept, or null for anything else (wrong version/action/length). */
fun parseCallAccept(data: ByteArray?): Int? {
    if (data == null || data.size < CALL_ACTION_PACKET_SIZE) return null
    if (data[0].toInt() != CALL_PACKET_VERSION || data[1].toInt() != CALL_ACTION_ACCEPT) return null
    return (data[2].toInt() and 0xFF) or ((data[3].toInt() and 0xFF) shl 8)
}

/** UTF-8 bytes of [s], at most [max], never splitting a multi-byte character. */
private fun utf8Truncated(s: String, max: Int): ByteArray {
    val bytes = s.encodeToByteArray()
    if (bytes.size <= max) return bytes
    var end = max
    // Back off continuation bytes (10xxxxxx) so the cut lands on a character start.
    while (end > 0 && (bytes[end].toInt() and 0xC0) == 0x80) end--
    return bytes.copyOf(end)
}
