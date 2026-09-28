package org.openapex.protocol

/**
 * Fragment framing for the nav and call characteristics, mirroring `ble_link.c`.
 *
 * Every write is a small fixed-size fragment rather than one large one. This is a workaround, not
 * a design: on the 2026-09-24 captures every `BluetoothGatt.requestMtu` came back
 * GATT_REQUEST_NOT_SUPPORTED while the terminal logged a successful exchange to 256 on its side,
 * and Android truncated every write to 20 bytes on the wire regardless. WRITE_TYPE_NO_RESPONSE
 * gives no delivery feedback, so a truncated write just reports success and vanishes. Chunking to
 * [FRAG_WRITE_BYTES] fits the BLE-spec default 23-byte ATT MTU on any stack, so correctness stops
 * depending on an exchange that lied.
 *
 * The terminal also accepts a whole packet in one write (DEVICE_INFO caps bit 1), which is what
 * iOS should use -- it negotiates 185 and honours it. That path is not wired up here yet.
 */

const val FRAG_HEADER_BYTES = 3

/** Payload bytes per fragment. FRAG_HEADER_BYTES + this must fit MTU 23 - 3 = 20. */
const val FRAG_CHUNK_BYTES = 17

const val FRAG_WRITE_BYTES = FRAG_HEADER_BYTES + FRAG_CHUNK_BYTES

val NAV_FRAG_COUNT = (RAW_NOTIF_PACKET_SIZE + FRAG_CHUNK_BYTES - 1) / FRAG_CHUNK_BYTES
val CALL_FRAG_COUNT = (CALL_EVENT_PACKET_SIZE + FRAG_CHUNK_BYTES - 1) / FRAG_CHUNK_BYTES

/** `[packet_id][frag_index][frag_count]` then up to [FRAG_CHUNK_BYTES] payload bytes. */
fun fragment(packet: ByteArray, packetId: Int, fragCount: Int): List<ByteArray> {
    val frames = mutableListOf<ByteArray>()
    var offset = 0
    var fragIndex = 0
    while (offset < packet.size) {
        val chunkLen = minOf(FRAG_CHUNK_BYTES, packet.size - offset)
        val frame = ByteArray(FRAG_HEADER_BYTES + chunkLen)
        frame[0] = packetId.toByte()
        frame[1] = fragIndex.toByte()
        frame[2] = fragCount.toByte()
        packet.copyInto(frame, FRAG_HEADER_BYTES, offset, offset + chunkLen)
        frames += frame
        offset += chunkLen
        fragIndex++
    }
    return frames
}

/** One fragment waiting to go out, and which characteristic it belongs to. */
class Frame(val call: Boolean, val bytes: ByteArray)

/**
 * What to send next, and in what order. Deliberately knows nothing about how sending works: the
 * platform layer owns the GATT calls and the one-operation-in-flight rule, and asks this only
 * what the next frame is.
 *
 * **Not thread-safe.** The Android client already holds a lock across every access; adding a
 * second, multiplatform one here would be a lock nobody needs and a source of a deadlock somebody
 * eventually finds.
 */
class FragmentQueue {
    private val pending = ArrayDeque<Frame>()

    /** Wraps 0-255, matching the firmware's uint8_t packet id. */
    var nextPacketId = 0
        private set

    val isEmpty: Boolean get() = pending.isEmpty()

    private fun takePacketId(): Int {
        val id = nextPacketId
        nextPacketId = (nextPacketId + 1) and 0xFF
        return id
    }

    /**
     * Queues a nav packet, dropping any nav fragments still waiting.
     *
     * A newer publish (~3 Hz) supersedes whatever of the previous packet has not gone out yet. The
     * terminal's reassembler keys off packet_id, so a half-sent old packet is harmless rather than
     * corrupting. Queued call fragments are kept: a ring must not be dropped by a nav update.
     */
    fun offerNav(packet: ByteArray): Int {
        val id = takePacketId()
        pending.removeAll { !it.call }
        pending.addAll(fragment(packet, id, NAV_FRAG_COUNT).map { Frame(call = false, bytes = it) })
        return id
    }

    /**
     * Queues a call event ahead of any nav fragments -- a ring must not wait behind a nav packet --
     * and drops any older call event, since only the newest matters (a CLEAR supersedes a queued
     * RING).
     */
    fun offerCall(packet: ByteArray): Int {
        val id = takePacketId()
        pending.removeAll { it.call }
        val frames = fragment(packet, id, CALL_FRAG_COUNT).map { Frame(call = true, bytes = it) }
        val firstNav = pending.indexOfFirst { !it.call }.let { if (it < 0) pending.size else it }
        pending.addAll(firstNav, frames)
        return id
    }

    fun peek(): Frame? = pending.firstOrNull()

    fun removeFirst() {
        pending.removeFirst()
    }
}
