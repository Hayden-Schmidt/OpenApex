package org.openapex.app

import android.content.Context
import android.util.Base64
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey
import org.openapex.protocol.Friend
import org.openapex.protocol.RiderIdentity
import org.openapex.store.RiderStoreCore
import org.openapex.store.SecureStore
import java.security.SecureRandom

/**
 * Android backing for the rider-mesh store. The logic is in [RiderStoreCore] (commonMain); this is
 * the Keystore-backed [SecureStore] under it plus the lock, so callers keep passing a [Context]
 * and nothing above here changed.
 */
private class EncryptedPrefsStore(context: Context) : SecureStore {
    private val prefs = EncryptedSharedPreferences.create(
        context.applicationContext,
        PREFS,
        MasterKey.Builder(context.applicationContext).setKeyScheme(MasterKey.KeyScheme.AES256_GCM).build(),
        EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV,
        EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM,
    )

    // SharedPreferences holds strings, so bytes are Base64'd here rather than in the shared
    // layer: the Keychain stores Data directly and should not inherit an encoding it never needed.
    //
    // The catches are for values written by the previous layout, which are still on any phone
    // that ran an earlier build. Friends were a StringSet, so getString() on that key throws
    // ClassCastException; the name was plain text, which is not valid Base64 and throws
    // IllegalArgumentException. Both read as absent, and both recover on their own -- the
    // terminal holds the friend list and syncRiderMesh merges it back on the next connect, and
    // the name falls back to the default until it is set again. The rider id and key are
    // unaffected: they were already Base64 under the same keys, so an identity survives, which
    // is the one thing here that cannot be regenerated without orphaning every friendship.
    override fun get(key: String): ByteArray? = try {
        prefs.getString(key, null)?.let { Base64.decode(it, Base64.NO_WRAP) }
    } catch (e: ClassCastException) {
        null
    } catch (e: IllegalArgumentException) {
        null
    }

    override fun put(key: String, value: ByteArray?) {
        prefs.edit().apply {
            if (value == null) remove(key) else putString(key, Base64.encodeToString(value, Base64.NO_WRAP))
        }.apply()
    }

    private companion object {
        const val PREFS = "rider_mesh_secure"
    }
}

object RiderStore {
    @Volatile private var core: RiderStoreCore? = null

    private fun core(c: Context): RiderStoreCore = core ?: synchronized(this) {
        core ?: RiderStoreCore(
            store = EncryptedPrefsStore(c),
            randomBytes = { n -> ByteArray(n).also(SecureRandom()::nextBytes) },
        ).also { core = it }
    }

    @Synchronized
    fun identity(c: Context): RiderIdentity = core(c).identity()

    @Synchronized
    fun setName(c: Context, name: String) = core(c).setName(name)

    @Synchronized
    fun friends(c: Context): List<Friend> = core(c).friends()

    @Synchronized
    fun setFriends(c: Context, list: List<Friend>) = core(c).setFriends(list)

    @Synchronized
    fun removeFriend(c: Context, id: ByteArray) = core(c).removeFriend(id)
}
