package org.openapex.ble

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue

private val C = TerminalCharacteristic.SETTINGS

class ControlQueueTest {

    /** Issues and completes whatever is queued, so a test can say what the link reported back. */
    private fun ControlQueue.run(vararg results: Boolean) = results.forEach {
        markStarted()
        complete(it)
    }

    @Test
    fun `a batch reports success once, after the last frame`() {
        val q = ControlQueue()
        val calls = mutableListOf<Boolean>()
        q.writeAll(C, listOf(byteArrayOf(1), byteArrayOf(2), byteArrayOf(3))) { calls += it }

        q.run(true, true)
        assertEquals(emptyList(), calls) // still going
        q.run(true)
        assertEquals(listOf(true), calls)
    }

    @Test
    fun `the first failure answers once and the rest of the batch is inert`() {
        val q = ControlQueue()
        val calls = mutableListOf<Boolean>()
        q.writeAll(C, listOf(byteArrayOf(1), byteArrayOf(2), byteArrayOf(3))) { calls += it }

        q.run(true, false)
        assertEquals(listOf(false), calls)
        // The remaining frame is still queued -- the pump has not reached it. Whether it is issued
        // or failed, the caller must not hear a second answer.
        q.failAll()
        assertEquals(listOf(false), calls)
    }

    @Test
    fun `an empty batch succeeds without queueing anything`() {
        val q = ControlQueue()
        var result: Boolean? = null
        q.writeAll(C, emptyList()) { result = it }
        assertEquals(true, result)
        assertTrue(q.isEmpty())
    }

    @Test
    fun `a read gets its value, and null when the read failed`() {
        val q = ControlQueue()
        var got: ByteArray? = byteArrayOf(9)
        q.read(C) { got = it }
        q.markStarted()
        q.complete(true, byteArrayOf(1, 2))
        assertEquals(listOf<Byte>(1, 2), got!!.toList())

        q.read(C) { got = it }
        q.markStarted()
        q.complete(false, byteArrayOf(1, 2)) // a value alongside a failure must not be handed out
        assertNull(got)
    }

    @Test
    fun `a disconnect answers everything, queued and in flight`() {
        val q = ControlQueue()
        val writes = mutableListOf<Boolean>()
        var read: ByteArray? = byteArrayOf(9)
        q.writeAll(C, listOf(byteArrayOf(1))) { writes += it }
        q.read(C) { read = it }
        q.markStarted() // the write is in flight when the link drops

        q.failAll()
        assertEquals(listOf(false), writes)
        assertNull(read)
        assertTrue(q.isEmpty())
    }

    @Test
    fun `a missing characteristic fails only the head`() {
        val q = ControlQueue()
        val calls = mutableListOf<String>()
        q.read(TerminalCharacteristic.AVATAR) { calls += "avatar=${it == null}" }
        q.read(TerminalCharacteristic.SETTINGS) { calls += "settings=${it == null}" }

        q.failHead()
        assertEquals(listOf("avatar=true"), calls)
        assertEquals(TerminalCharacteristic.SETTINGS, q.peek()!!.char)
    }

    @Test
    fun `a completion with nothing in flight is ignored`() {
        val q = ControlQueue()
        val calls = mutableListOf<Boolean>()
        q.writeAll(C, listOf(byteArrayOf(1))) { calls += it }
        // A CCCD write the queue never issued lands here; it must not consume a queued op.
        q.complete(true)
        assertEquals(emptyList(), calls)
        assertEquals(C, q.peek()!!.char)
    }

    @Test
    fun `operations are issued in the order they were queued`() {
        val q = ControlQueue()
        val order = mutableListOf<TerminalCharacteristic>()
        q.read(TerminalCharacteristic.DEVICE_INFO) {}
        q.writeAll(TerminalCharacteristic.SETTINGS, listOf(byteArrayOf(1))) {}
        q.subscribe(TerminalCharacteristic.FRIENDS)
        while (!q.isEmpty()) {
            order += q.peek()!!.char
            q.markStarted()
            q.complete(true)
        }
        assertEquals(
            listOf(TerminalCharacteristic.DEVICE_INFO, TerminalCharacteristic.SETTINGS, TerminalCharacteristic.FRIENDS),
            order,
        )
    }
}
