package org.openapex.app

import org.openapex.protocol.*
import android.content.Context
import android.util.Base64
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey
import java.security.SecureRandom

/**
 * Phone-side persistence for the rider mesh (spec §5.7): this rider's identity, generated on first
 * use, and the friend list, which the phone owns and every terminal it connects to caches. Both
 * hold rider keys, so they live in EncryptedSharedPreferences (Keystore-backed).
 */
object RiderStore {
    private const val PREFS = "rider_mesh_secure"
    private const val KEY_ID = "id"
    private const val KEY_KEY = "key"
    private const val KEY_NAME = "name"
    private const val KEY_FRIENDS = "friends"
    private const val DEFAULT_NAME = "Rider"

    @Volatile private var cached: android.content.SharedPreferences? = null

    private fun prefs(c: Context) = cached ?: synchronized(this) {
        cached ?: EncryptedSharedPreferences.create(
            c.applicationContext,
            PREFS,
            MasterKey.Builder(c.applicationContext).setKeyScheme(MasterKey.KeyScheme.AES256_GCM).build(),
            EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV,
            EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM,
        ).also { cached = it }
    }

    @Synchronized
    fun identity(c: Context): RiderIdentity {
        val p = prefs(c)
        val id = p.getString(KEY_ID, null)?.let(::unb64)
        val key = p.getString(KEY_KEY, null)?.let(::unb64)
        if (id != null && key != null) {
            return RiderIdentity(id, key, p.getString(KEY_NAME, DEFAULT_NAME)!!)
        }
        val rnd = SecureRandom()
        val newId = ByteArray(RiderIdentity.ID_LEN).also(rnd::nextBytes)
        val newKey = ByteArray(RiderIdentity.KEY_LEN).also(rnd::nextBytes)
        p.edit().putString(KEY_ID, b64(newId)).putString(KEY_KEY, b64(newKey)).apply()
        return RiderIdentity(newId, newKey, p.getString(KEY_NAME, DEFAULT_NAME)!!)
    }

    fun setName(c: Context, name: String) = prefs(c).edit().putString(KEY_NAME, name).apply()

    @Synchronized
    fun friends(c: Context): List<Friend> =
        prefs(c).getStringSet(KEY_FRIENDS, emptySet())!!.mapNotNull { Friend.decode(unb64(it)) }

    @Synchronized
    fun setFriends(c: Context, list: List<Friend>) =
        prefs(c).edit().putStringSet(KEY_FRIENDS, list.map { b64(it.encode()) }.toSet()).apply()

    fun removeFriend(c: Context, id: ByteArray) =
        setFriends(c, friends(c).filterNot { it.id.contentEquals(id) })

    private fun b64(b: ByteArray) = Base64.encodeToString(b, Base64.NO_WRAP)
    private fun unb64(s: String) = Base64.decode(s, Base64.NO_WRAP)
}
