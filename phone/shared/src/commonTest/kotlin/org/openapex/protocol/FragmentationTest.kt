package org.openapex.protocol

import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue

class FragmentationTest {

    private fun navPacket(marker: Byte) = ByteArray(RAW_NOTIF_PACKET_SIZE) { marker }

    @Test
    fun `a nav packet splits into the frames the firmware reassembles`() {
        val packet = ByteArray(RAW_NOTIF_PACKET_SIZE) { (it and 0xFF).toByte() }
        val frames = fragment(packet, packetId = 0x2A, fragCount = NAV_FRAG_COUNT)

        assertEquals(13, NAV_FRAG_COUNT)
        assertEquals(13, frames.size)
        // 12 full chunks of 17 leaves 6 bytes in the last: 210 = 12 * 17 + 6.
        frames.dropLast(1).forEach { assertEquals(FRAG_WRITE_BYTES, it.size) }
        assertEquals(FRAG_HEADER_BYTES + 6, frames.last().size)

        frames.forEachIndexed { i, f ->
            assertEquals(0x2A, f[0].toInt() and 0xFF)
            assertEquals(i, f[1].toInt() and 0xFF)
            assertEquals(NAV_FRAG_COUNT, f[2].toInt() and 0xFF)
        }

        // Reassembling the payloads must give the original back, byte for byte.
        val rebuilt = frames.fold(ByteArray(0)) { acc, f -> acc + f.copyOfRange(FRAG_HEADER_BYTES, f.size) }
        assertContentEquals(packet, rebuilt)
    }

    @Test
    fun `the fragment write fits the default ATT MTU`() {
        // The whole reason this scheme exists. If it stops holding, writes are silently truncated
        // on a stack that does not honour the MTU it agreed.
        assertTrue(FRAG_WRITE_BYTES <= 20)
    }

    @Test
    fun `a new nav packet supersedes the unsent one but spares queued call frames`() {
        val q = FragmentQueue()
        q.offerCall(ByteArray(CALL_EVENT_PACKET_SIZE) { 0x11 })
        q.offerNav(navPacket(0x22))
        val second = q.offerNav(navPacket(0x33))

        // Drain and count what survived, by characteristic.
        var call = 0
        var nav = 0
        val navIds = mutableSetOf<Int>()
        while (true) {
            val f = q.peek() ?: break
            if (f.call) call++ else { nav++; navIds += f.bytes[0].toInt() and 0xFF }
            q.removeFirst()
        }
        assertEquals(CALL_FRAG_COUNT, call)
        assertEquals(NAV_FRAG_COUNT, nav)
        assertEquals(setOf(second), navIds) // only the newest nav packet is left
    }

    @Test
    fun `a call event goes out ahead of queued nav fragments`() {
        val q = FragmentQueue()
        q.offerNav(navPacket(0x44))
        q.offerCall(ByteArray(CALL_EVENT_PACKET_SIZE) { 0x55 })

        repeat(CALL_FRAG_COUNT) {
            assertTrue(q.peek()!!.call, "call fragments must lead")
            q.removeFirst()
        }
        assertEquals(false, q.peek()!!.call)
    }

    @Test
    fun `only the newest call event survives`() {
        val q = FragmentQueue()
        q.offerCall(ByteArray(CALL_EVENT_PACKET_SIZE) { 0x66 })
        val clear = q.offerCall(ByteArray(CALL_EVENT_PACKET_SIZE) { 0x77 })
        var frames = 0
        while (q.peek() != null) {
            assertEquals(clear, q.peek()!!.bytes[0].toInt() and 0xFF)
            frames++
            q.removeFirst()
        }
        assertEquals(CALL_FRAG_COUNT, frames)
    }

    @Test
    fun `the packet id wraps like the firmware's uint8_t`() {
        val q = FragmentQueue()
        var last = -1
        repeat(258) { last = q.offerNav(navPacket(0)) }
        assertEquals(257 and 0xFF, last)
        assertEquals(1, last) // 258 packets starting at 0: the last id is 257 mod 256
    }

    @Test
    fun `an empty queue has nothing to send`() {
        val q = FragmentQueue()
        assertTrue(q.isEmpty)
        assertNull(q.peek())
        q.offerNav(navPacket(0))
        assertTrue(!q.isEmpty)
    }
}
