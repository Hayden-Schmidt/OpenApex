package org.openapex.store

/**
 * A small secrets store, backed by EncryptedSharedPreferences on Android and the Keychain on iOS.
 *
 * Byte arrays rather than strings, because that is what both backends actually hold and what the
 * data actually is: rider ids and AES keys. Android needs Base64 to put bytes in
 * SharedPreferences, but that is Android's problem and stays in its implementation rather than
 * becoming an encoding every platform has to agree on.
 */
interface SecureStore {
    fun get(key: String): ByteArray?

    /** A null [value] removes the key. */
    fun put(key: String, value: ByteArray?)
}
