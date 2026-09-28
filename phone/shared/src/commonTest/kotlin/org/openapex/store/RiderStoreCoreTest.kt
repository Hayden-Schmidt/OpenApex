package org.openapex.store

import org.openapex.protocol.Friend
import org.openapex.protocol.RiderIdentity
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertTrue

private class FakeStore : SecureStore {
    val map = mutableMapOf<String, ByteArray>()
    override fun get(key: String): ByteArray? = map[key]
    override fun put(key: String, value: ByteArray?) {
        if (value == null) map.remove(key) else map[key] = value
    }
}

class RiderStoreCoreTest {

    // Counts up so every generated identity is distinguishable, and so a test can say exactly
    // which bytes it expects.
    private fun counting(): (Int) -> ByteArray {
        var seed = 1
        return { n -> ByteArray(n) { (seed + it).toByte() }.also { seed += 100 } }
    }

    private fun friend(tag: Byte, name: String = "Sam") = Friend(
        id = ByteArray(RiderIdentity.ID_LEN) { tag },
        key = ByteArray(RiderIdentity.KEY_LEN) { tag },
        name = name,
        avatar = 3,
        accentRgb = 0x00FFE8,
        art = null,
    )

    @Test
    fun `an identity is generated once and then reused`() {
        val store = FakeStore()
        val core = RiderStoreCore(store, counting())

        val first = core.identity()
        assertEquals(RiderIdentity.ID_LEN, first.id.size)
        assertEquals(RiderIdentity.KEY_LEN, first.key.size)
        assertEquals("Rider", first.name)

        // Second call must NOT mint a new one: the rider id is how friends recognise this phone,
        // so regenerating it would silently orphan every friendship already made.
        val second = RiderStoreCore(store, counting()).identity()
        assertContentEquals(first.id, second.id)
        assertContentEquals(first.key, second.key)
    }

    @Test
    fun `a half-written identity is regenerated rather than half-used`() {
        val store = FakeStore()
        store.put("id", ByteArray(RiderIdentity.ID_LEN) { 9 })
        // No key stored -- an interrupted first run.
        val id = RiderStoreCore(store, counting()).identity()
        assertTrue(!id.id.contentEquals(ByteArray(RiderIdentity.ID_LEN) { 9 }))
        assertEquals(RiderIdentity.KEY_LEN, id.key.size)
    }

    @Test
    fun `the name survives a round trip and defaults until set`() {
        val store = FakeStore()
        val core = RiderStoreCore(store, counting())
        assertEquals("Rider", core.identity().name)
        core.setName("Hayden")
        assertEquals("Hayden", core.identity().name)
    }

    @Test
    fun `friends round-trip through the fixed-width blob`() {
        val store = FakeStore()
        val core = RiderStoreCore(store, counting())
        assertEquals(emptyList(), core.friends())

        val list = listOf(friend(1, "A"), friend(2, "B"), friend(3, "C"))
        core.setFriends(list)
        assertEquals(3 * Friend.RECORD_LEN, store.get("friends")!!.size)

        val back = core.friends()
        assertEquals(listOf("A", "B", "C"), back.map { it.name })
        assertContentEquals(list[1].id, back[1].id)
    }

    @Test
    fun `a truncated blob reads as no friends, not as a partial list`() {
        val store = FakeStore()
        val core = RiderStoreCore(store, counting())
        core.setFriends(listOf(friend(1), friend(2)))
        val whole = store.get("friends")!!
        store.put("friends", whole.copyOfRange(0, whole.size - 3))
        assertEquals(emptyList(), core.friends())
    }

    @Test
    fun `removing a friend leaves the others`() {
        val store = FakeStore()
        val core = RiderStoreCore(store, counting())
        core.setFriends(listOf(friend(1, "A"), friend(2, "B"), friend(3, "C")))
        core.removeFriend(ByteArray(RiderIdentity.ID_LEN) { 2 })
        assertEquals(listOf("A", "C"), core.friends().map { it.name })
    }
}
