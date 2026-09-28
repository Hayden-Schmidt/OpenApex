package org.openapex.store

import org.openapex.protocol.Friend
import org.openapex.protocol.RiderIdentity

/**
 * Rider-mesh persistence (spec §5.7): this rider's identity, generated on first use, and the
 * friend list the phone owns and every terminal it connects to caches. Both hold rider keys, so
 * the backing [SecureStore] is Keystore- or Keychain-backed.
 *
 * Randomness is injected rather than taken from the platform. Identity generation is then the
 * same code everywhere and a test can pin the bytes it produces, which is the only way to assert
 * anything about a function whose whole job is to be unpredictable.
 *
 * **Not thread-safe**, like the rest of the shared layer. The platform facade serialises access.
 */
class RiderStoreCore(
    private val store: SecureStore,
    private val randomBytes: (Int) -> ByteArray,
) : RiderMeshStore {
    override fun identity(): RiderIdentity {
        val id = store.get(KEY_ID)
        val key = store.get(KEY_KEY)
        if (id != null && key != null) {
            return RiderIdentity(id, key, name())
        }
        val newId = randomBytes(RiderIdentity.ID_LEN)
        val newKey = randomBytes(RiderIdentity.KEY_LEN)
        store.put(KEY_ID, newId)
        store.put(KEY_KEY, newKey)
        return RiderIdentity(newId, newKey, name())
    }

    private fun name(): String = store.get(KEY_NAME)?.decodeToString() ?: DEFAULT_NAME

    fun setName(name: String) = store.put(KEY_NAME, name.encodeToByteArray())

    /**
     * Friend records are a fixed [Friend.RECORD_LEN] bytes, so the list is their concatenation --
     * no separator, no length prefix, nothing to get out of step. A trailing partial record means
     * a truncated write, and the whole list is treated as unreadable rather than half-restored.
     */
    override fun friends(): List<Friend> {
        val blob = store.get(KEY_FRIENDS) ?: return emptyList()
        if (blob.size % Friend.RECORD_LEN != 0) return emptyList()
        return (0 until blob.size / Friend.RECORD_LEN).mapNotNull { i ->
            Friend.decode(blob.copyOfRange(i * Friend.RECORD_LEN, (i + 1) * Friend.RECORD_LEN))
        }
    }

    override fun setFriends(list: List<Friend>) {
        val blob = ByteArray(list.size * Friend.RECORD_LEN)
        list.forEachIndexed { i, f -> f.encode().copyInto(blob, i * Friend.RECORD_LEN) }
        store.put(KEY_FRIENDS, blob)
    }

    fun removeFriend(id: ByteArray) = setFriends(friends().filterNot { it.id.contentEquals(id) })

    private companion object {
        const val KEY_ID = "id"
        const val KEY_KEY = "key"
        const val KEY_NAME = "name"
        const val KEY_FRIENDS = "friends"
        const val DEFAULT_NAME = "Rider"
    }
}
