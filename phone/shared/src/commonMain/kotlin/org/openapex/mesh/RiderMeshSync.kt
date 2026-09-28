package org.openapex.mesh

import org.openapex.protocol.Friend
import org.openapex.protocol.RiderMesh
import org.openapex.store.RiderMeshStore

/**
 * Rider mesh sync on connect (spec §5.7): push this rider's identity, read the terminal's friends
 * (it may have made some with no phone connected), merge them into ours, write the merged list
 * back, then subscribe so friendships made later reach us too.
 *
 * The terminal is read one record at a time behind a cursor write, so this is a state machine
 * spread across callbacks rather than a loop. That is exactly the part worth having under test
 * without a phone in the room, and the part iOS needs unchanged.
 *
 * **Not thread-safe.** Every entry point must be called from the link's own callback thread.
 */
class RiderMeshSync(
    private val store: RiderMeshStore,
    private val link: MeshTransport,
) {
    /** Call once the terminal's services are discovered. */
    fun onConnected() {
        link.writeAll(MeshCharacteristic.IDENTITY, listOf(store.identity().encode())) { ok ->
            link.log("identity push ok=$ok")
        }
        readTerminalFriends(0, mutableListOf())
    }

    private fun readTerminalFriends(index: Int, acc: MutableList<Friend>) {
        link.writeAll(MeshCharacteristic.FRIENDS, listOf(RiderMesh.cursor(index))) { ok ->
            if (!ok) return@writeAll // old firmware or link gone; next connect retries
            link.read(MeshCharacteristic.FRIENDS) { rec ->
                val f = rec?.takeIf { it.isNotEmpty() }?.let(Friend::decode)
                if (f != null && index < RiderMesh.FRIEND_CAP) {
                    acc += f
                    readTerminalFriends(index + 1, acc)
                } else if (rec != null) {
                    // An empty record is the terminal saying "that is all of them".
                    mergeAndPush(acc)
                }
            }
        }
    }

    private fun mergeAndPush(terminal: List<Friend>) {
        val merged = RiderMesh.merge(store.friends(), terminal)
        store.setFriends(merged)
        link.log("friends sync: terminal=${terminal.size} merged=${merged.size}")
        pushFriends(merged)
        link.subscribe(MeshCharacteristic.FRIENDS)
    }

    /** Pushes the phone's friend list again, e.g. after the rider removes one in the app. */
    fun pushFriends(list: List<Friend> = store.friends()) = link.writeAll(
        MeshCharacteristic.FRIENDS, listOf(RiderMesh.clear()) + list.map(RiderMesh::upsert),
    ) { ok -> link.log("friends push ok=$ok") }

    /** A friendship completed on the terminal: `[op=UPSERT][record]`. */
    fun onFriendNotify(value: ByteArray) {
        if (value.isEmpty() || value[0].toInt() != RiderMesh.OP_UPSERT) return
        val f = Friend.decode(value.copyOfRange(1, value.size)) ?: return
        link.log("terminal added friend ${f.name}")
        store.setFriends(RiderMesh.merge(store.friends(), listOf(f)))
    }
}
