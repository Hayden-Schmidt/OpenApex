package org.openapex.androidauto

/**
 * Rider mesh wire formats for the identity (0x0008) and friends (0x0009) characteristics,
 * mirroring firmware/main/mesh_proto.h (docs/OpenApex_SPEC.md §5.7). Pure Kotlin, no Android
 * types, so it can move to KMP commonMain as is.
 */
class RiderIdentity(val id: ByteArray, val key: ByteArray, val name: String) {
    init {
        require(id.size == ID_LEN && key.size == KEY_LEN)
    }

    /** `[ver=1][rider_id 8][rider_key 16][name_len][name <= 12 B UTF-8]` */
    fun encode(): ByteArray {
        val n = RiderMesh.nameBytes(name)
        return byteArrayOf(VERSION.toByte()) + id + key + byteArrayOf(n.size.toByte()) + n
    }

    companion object {
        const val VERSION = 1
        const val ID_LEN = 8
        const val KEY_LEN = 16
    }
}

data class Friend(
    val id: ByteArray,
    val key: ByteArray,
    val name: String,
    val avatar: Int,
    val accentRgb: Int,
    /** 80-byte custom art, or null for a preset avatar. */
    val art: ByteArray?,
) {
    // Hand-rolled rather than "%02x".format(it): String.format is JVM-only, and this is
    // commonMain.
    val idHex: String get() = id.joinToString("") { b ->
        val v = b.toInt() and 0xFF
        "0123456789abcdef"[v shr 4].toString() + "0123456789abcdef"[v and 0x0F]
    }

    /** Same rider with the same look; ByteArray fields make the generated equals() useless. */
    fun sameAs(o: Friend) = id.contentEquals(o.id) && key.contentEquals(o.key) && name == o.name &&
        avatar == o.avatar && accentRgb == o.accentRgb &&
        (art ?: ByteArray(0)).contentEquals(o.art ?: ByteArray(0))

    /**
     * Fixed 122 bytes: rider_id 8 | rider_key 16 | avatar | accent RGB 3 | flags | name_len |
     * name 12 (zero-padded) | art 80 (zeros unless flags bit0).
     */
    fun encode(): ByteArray {
        val out = ByteArray(RECORD_LEN)
        id.copyInto(out, 0)
        key.copyInto(out, 8)
        out[24] = avatar.toByte()
        out[25] = (accentRgb shr 16).toByte()
        out[26] = (accentRgb shr 8).toByte()
        out[27] = accentRgb.toByte()
        out[28] = if (art != null) FLAG_CUSTOM.toByte() else 0
        val n = RiderMesh.nameBytes(name)
        out[29] = n.size.toByte()
        n.copyInto(out, 30)
        art?.copyInto(out, 42)
        return out
    }

    companion object {
        const val RECORD_LEN = 122
        const val ART_LEN = 80
        private const val FLAG_CUSTOM = 0x01

        fun decode(b: ByteArray): Friend? {
            if (b.size != RECORD_LEN) return null
            val nameLen = b[29].toInt() and 0xFF
            if (nameLen > RiderMesh.NAME_MAX) return null
            val custom = (b[28].toInt() and FLAG_CUSTOM) != 0
            return Friend(
                id = b.copyOfRange(0, 8),
                key = b.copyOfRange(8, 24),
                name = b.copyOfRange(30, 30 + nameLen).decodeToString(),
                avatar = b[24].toInt() and 0xFF,
                accentRgb = ((b[25].toInt() and 0xFF) shl 16) or ((b[26].toInt() and 0xFF) shl 8) or
                    (b[27].toInt() and 0xFF),
                art = if (custom) b.copyOfRange(42, 42 + ART_LEN) else null,
            )
        }
    }
}

object RiderMesh {
    const val NAME_MAX = 12
    const val FRIEND_CAP = 16

    // Friends characteristic write ops (firmware mesh.c).
    const val OP_UPSERT = 1
    const val OP_REMOVE = 2
    const val OP_CLEAR = 3
    const val OP_CURSOR = 4

    /** UTF-8, cut to [NAME_MAX] bytes without splitting a character. */
    fun nameBytes(name: String): ByteArray {
        var b = name.encodeToByteArray()
        var end = minOf(b.size, NAME_MAX)
        while (end > 0 && end < b.size && (b[end].toInt() and 0xC0) == 0x80) end--
        if (end < b.size) b = b.copyOf(end)
        return b
    }

    fun upsert(f: Friend) = byteArrayOf(OP_UPSERT.toByte()) + f.encode()
    fun remove(id: ByteArray) = byteArrayOf(OP_REMOVE.toByte()) + id
    fun clear() = byteArrayOf(OP_CLEAR.toByte())
    fun cursor(index: Int) = byteArrayOf(OP_CURSOR.toByte(), index.toByte())

    /**
     * The phone list after learning [terminal]'s. Friends made on the terminal while no phone was
     * connected exist only there, so they are added; a rider on both takes the terminal's copy
     * (newer look). Unfriending is phone-only, so nothing is dropped. Capped at [FRIEND_CAP].
     */
    fun merge(phone: List<Friend>, terminal: List<Friend>): List<Friend> {
        val out = phone.toMutableList()
        for (t in terminal) {
            val i = out.indexOfFirst { it.id.contentEquals(t.id) }
            if (i >= 0) out[i] = t else out += t
        }
        return out.take(FRIEND_CAP)
    }
}
