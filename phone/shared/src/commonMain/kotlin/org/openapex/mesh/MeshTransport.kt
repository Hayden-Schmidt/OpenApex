package org.openapex.mesh

/** The two rider-mesh characteristics (spec §5.7). */
enum class MeshCharacteristic { IDENTITY, FRIENDS }

/**
 * What [RiderMeshSync] needs from a terminal link, and nothing more. Callback-shaped rather than
 * suspending because the Android client is still main-looper Handlers throughout; converting the
 * queue pump to coroutines is its own step and this interface changes shape when it happens.
 */
interface MeshTransport {
    /**
     * Writes [frames] in order, with response. The first failure drops the rest of the batch and
     * calls [done] once with false; otherwise [done] fires once, after the last frame.
     */
    fun writeAll(char: MeshCharacteristic, frames: List<ByteArray>, done: (Boolean) -> Unit)

    /** Reads one value; [done] gets null when the read failed. */
    fun read(char: MeshCharacteristic, done: (ByteArray?) -> Unit)

    /** Subscribes to notifications, so friendships made on the terminal later reach the phone. */
    fun subscribe(char: MeshCharacteristic)

    fun log(message: String)
}
