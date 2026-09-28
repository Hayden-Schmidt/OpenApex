package org.openapex.ble

/**
 * A settings / device-info / avatar / rider-mesh operation (spec §5.6, §5.7). These are rare and
 * user-driven, and they run ahead of nav fragments in the pump: a delayed nav packet is superseded
 * by the next publish anyway, while a dropped settings write is just lost.
 */
sealed class ControlOp {
    abstract val char: TerminalCharacteristic

    class Write(
        override val char: TerminalCharacteristic,
        val value: ByteArray,
        val done: (Boolean) -> Unit,
    ) : ControlOp()

    class Read(
        override val char: TerminalCharacteristic,
        val done: (ByteArray?) -> Unit,
    ) : ControlOp()

    class Subscribe(override val char: TerminalCharacteristic) : ControlOp()
}

/**
 * Ordering and completion for control operations, with one in flight at a time. Both platforms
 * allow a single outstanding GATT operation, so the rule is not an Android quirk being ported.
 *
 * What is here is everything that is decided rather than performed: what runs next, how a batch
 * collapses on failure, and which callback a completion belongs to. Issuing the operation and
 * knowing it finished stays with the platform.
 *
 * Every operation is answered exactly once, including on disconnect ([failAll]) and against
 * firmware too old to have the characteristic ([failHead]). A caller left waiting forever is worse
 * than one told no -- the app has no timeout above this.
 *
 * **Not thread-safe.** The caller serialises, since on Android this queue and the fragment queue
 * share one in-flight slot and must be consistent together.
 */
class ControlQueue {
    private val ops = ArrayDeque<ControlOp>()
    private var inFlight: ControlOp? = null

    /**
     * Queues [frames] as writes in order. The first failure drops the rest of the batch, so [done]
     * fires exactly once either way: at the first failure, or after the last frame.
     *
     * Partial application is the point. Settings arrive as several writes and the terminal applies
     * each as it lands; if frame 2 of 5 fails there is no undo, so the remaining three are dropped
     * rather than written onto a terminal whose state is already unknown.
     */
    fun writeAll(char: TerminalCharacteristic, frames: List<ByteArray>, done: (Boolean) -> Unit) {
        if (frames.isEmpty()) {
            done(true)
            return
        }
        var failed = false
        frames.forEachIndexed { i, frame ->
            ops.addLast(ControlOp.Write(char, frame) { ok ->
                if (failed) return@Write
                if (!ok) failed = true
                if (!ok || i == frames.lastIndex) done(ok)
            })
        }
    }

    fun read(char: TerminalCharacteristic, done: (ByteArray?) -> Unit) {
        ops.addLast(ControlOp.Read(char, done))
    }

    fun subscribe(char: TerminalCharacteristic) {
        ops.addLast(ControlOp.Subscribe(char))
    }

    /** The next operation to issue, or null when there is none. Does not remove it. */
    fun peek(): ControlOp? = ops.firstOrNull()

    /** The platform issued [peek] successfully: it becomes the one in flight. */
    fun markStarted() {
        inFlight = ops.removeFirst()
    }

    /**
     * Completes whatever is in flight with [value] (a read) or [ok] (a write). A completion with
     * nothing in flight is ignored: a stack can report a descriptor write the queue never issued.
     */
    fun complete(ok: Boolean, value: ByteArray? = null) {
        val op = inFlight ?: return
        inFlight = null
        when (op) {
            is ControlOp.Write -> op.done(ok)
            is ControlOp.Read -> op.done(if (ok) value else null)
            is ControlOp.Subscribe -> Unit
        }
    }

    /** The head's characteristic is missing -- firmware older than the spec. Fails and drops it. */
    fun failHead() {
        ops.removeFirstOrNull()?.let(::fail)
    }

    /**
     * The link dropped. Everything queued and anything in flight is failed, because a link lost
     * mid-operation never delivers its callback and a caller would otherwise wait forever.
     */
    fun failAll() {
        val all = listOfNotNull(inFlight) + ops
        inFlight = null
        ops.clear()
        all.forEach(::fail)
    }

    fun isEmpty(): Boolean = ops.isEmpty() && inFlight == null

    private fun fail(op: ControlOp) = when (op) {
        is ControlOp.Write -> op.done(false)
        is ControlOp.Read -> op.done(null)
        is ControlOp.Subscribe -> Unit
    }
}
