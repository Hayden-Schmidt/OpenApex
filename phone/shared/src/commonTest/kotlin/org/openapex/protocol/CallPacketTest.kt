package org.openapex.protocol

import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.Test

class CallPacketTest {

    @Test
    fun `ring packet matches the firmware golden bytes`() {
        // Same packet firmware/test_host/call_test.c decodes: RING, id 0x1234, "Jane Doe", "+64 27 123".
        val p = packCallEvent(CALL_EVENT_RING, 0x1234, "Jane Doe", "+64 27 123")
        assertEquals(CALL_EVENT_PACKET_SIZE, p.size)
        assertEquals(1, p[0].toInt())
        assertEquals(CALL_EVENT_RING, p[1].toInt())
        assertEquals(0x34, p[2].toInt() and 0xFF)
        assertEquals(0x12, p[3].toInt() and 0xFF)
        assertEquals("Jane Doe", p.decodeToString(4, 12))
        assertEquals(0, p[12].toInt())
        assertEquals("+64 27 123", p.decodeToString(44, 54))
        assertEquals(0, p[54].toInt())
    }

    @Test
    fun `null caller is an empty field`() {
        val p = packCallEvent(CALL_EVENT_CLEAR, 7, null)
        assertEquals(CALL_EVENT_CLEAR, p[1].toInt())
        assertEquals(0, p[4].toInt())
    }

    @Test
    fun `long caller is cut on a character boundary`() {
        // 39 ASCII bytes then a 2-byte character: it does not fit the 40th byte, so it is dropped
        // whole rather than leaving half a sequence.
        val caller = "A".repeat(39) + "é"
        val p = packCallEvent(CALL_EVENT_RING, 1, caller)
        assertEquals('A'.code, p[4 + 38].toInt())
        assertEquals(0, p[4 + 39].toInt())
    }

    @Test
    fun `accept parses and rejects malformed`() {
        assertEquals(0x1234, parseCallAccept(byteArrayOf(1, 1, 0x34, 0x12)))
        assertNull(parseCallAccept(byteArrayOf(2, 1, 0x34, 0x12)))
        assertNull(parseCallAccept(byteArrayOf(1, 9, 0x34, 0x12)))
        assertNull(parseCallAccept(byteArrayOf(1, 1, 0x34)))
        assertNull(parseCallAccept(null))
    }
}
