package org.openapex.mesh

import org.openapex.protocol.Friend
import org.openapex.protocol.RiderIdentity
import org.openapex.protocol.RiderMesh
import org.openapex.store.RiderMeshStore
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

private fun friend(tag: Byte, name: String) = Friend(
    id = ByteArray(RiderIdentity.ID_LEN) { tag },
    key = ByteArray(RiderIdentity.KEY_LEN) { tag },
    name = name,
    avatar = 1,
    accentRgb = 0x00FFE8,
    art = null,
)

private class FakeStore(var list: List<Friend> = emptyList()) : RiderMeshStore {
    override fun identity() = RiderIdentity(ByteArray(RiderIdentity.ID_LEN) { 7 }, ByteArray(RiderIdentity.KEY_LEN) { 7 }, "Me")
    override fun friends() = list
    override fun setFriends(list: List<Friend>) { this.list = list }
}

/**
 * Stands in for the terminal: serves [held] one record per cursor, an empty record past the end,
 * and records every write so the test can assert what was pushed back.
 */
private class FakeLink(private val held: List<Friend>) : MeshTransport {
    val writes = mutableListOf<Pair<MeshCharacteristic, ByteArray>>()
    val subscribed = mutableListOf<MeshCharacteristic>()
    val logs = mutableListOf<String>()
    var writeFails = false
    private var cursor = -1

    override fun writeAll(char: MeshCharacteristic, frames: List<ByteArray>, done: (Boolean) -> Unit) {
        if (writeFails) { done(false); return }
        frames.forEach { f ->
            writes += char to f
            if (char == MeshCharacteristic.FRIENDS && f.size == 2 && f[0].toInt() == RiderMesh.OP_CURSOR) {
                cursor = f[1].toInt()
            }
        }
        done(true)
    }

    override fun read(char: MeshCharacteristic, done: (ByteArray?) -> Unit) =
        done(held.getOrNull(cursor)?.encode() ?: ByteArray(0))

    override fun subscribe(char: MeshCharacteristic) { subscribed += char }
    override fun log(message: String) { logs += message }

    fun pushedFriends(): List<String> = writes
        .filter { it.first == MeshCharacteristic.FRIENDS && it.second[0].toInt() == RiderMesh.OP_UPSERT }
        .mapNotNull { Friend.decode(it.second.copyOfRange(1, it.second.size))?.name }
}

class RiderMeshSyncTest {

    @Test
    fun `a full sync pushes identity, drains the terminal, merges and pushes back`() {
        val store = FakeStore(listOf(friend(1, "Phone")))
        val link = FakeLink(listOf(friend(2, "Terminal")))
        RiderMeshSync(store, link).onConnected()

        assertEquals(1, link.writes.count { it.first == MeshCharacteristic.IDENTITY })
        // Cursor 0 and 1: the second read returns empty, which is the terminal saying that is all.
        assertEquals(listOf(0, 1), link.writes.filter {
            it.first == MeshCharacteristic.FRIENDS && it.second[0].toInt() == RiderMesh.OP_CURSOR
        }.map { it.second[1].toInt() })

        assertEquals(listOf("Phone", "Terminal"), store.list.map { it.name })
        assertEquals(store.list.map { it.name }, link.pushedFriends())
        // The push must clear first, or the terminal keeps friends the phone has since removed.
        // Scoped to FRIENDS: an identity record also starts with a 1, which is OP_UPSERT's value.
        val friendOps = link.writes.filter { it.first == MeshCharacteristic.FRIENDS }.map { it.second[0].toInt() }
        assertTrue(friendOps.indexOf(RiderMesh.OP_CLEAR) in 0 until friendOps.indexOf(RiderMesh.OP_UPSERT))
        assertEquals(listOf(MeshCharacteristic.FRIENDS), link.subscribed)
    }

    @Test
    fun `an empty terminal still gets the phone's list`() {
        val store = FakeStore(listOf(friend(1, "Phone")))
        val link = FakeLink(emptyList())
        RiderMeshSync(store, link).onConnected()
        assertEquals(listOf("Phone"), link.pushedFriends())
    }

    @Test
    fun `a terminal that rejects the cursor write is left alone until the next connect`() {
        val store = FakeStore(listOf(friend(1, "Phone")))
        val link = FakeLink(listOf(friend(2, "Terminal")))
        link.writeFails = true
        RiderMeshSync(store, link).onConnected()

        // Old firmware has no cursor op. Nothing was read, so nothing may be merged or stored:
        // treating "no answer" as "no friends" would wipe the list on every stale-firmware connect.
        assertEquals(listOf("Phone"), store.list.map { it.name })
        assertTrue(link.subscribed.isEmpty())
    }

    @Test
    fun `a friendship made on the terminal is merged in`() {
        val store = FakeStore(listOf(friend(1, "Phone")))
        val sync = RiderMeshSync(store, FakeLink(emptyList()))
        sync.onFriendNotify(RiderMesh.upsert(friend(2, "New")))
        assertEquals(listOf("Phone", "New"), store.list.map { it.name })
    }

    @Test
    fun `a notify that is not an upsert changes nothing`() {
        val store = FakeStore(listOf(friend(1, "Phone")))
        val sync = RiderMeshSync(store, FakeLink(emptyList()))
        sync.onFriendNotify(RiderMesh.clear())
        sync.onFriendNotify(ByteArray(0))
        assertEquals(listOf("Phone"), store.list.map { it.name })
    }
}
