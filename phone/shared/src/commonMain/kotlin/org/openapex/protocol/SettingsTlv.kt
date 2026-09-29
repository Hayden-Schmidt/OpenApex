package org.openapex.protocol

/**
 * Terminal UI settings, mirroring firmware/main/settings.h. Wire format and defaults are
 * docs/reference/settings_and_avatar.md; [SettingsTlv.DEFAULTS_FIXTURE] is the spec's golden fixture.
 */
data class TerminalSettings(
    val accentRgb: Int = 0x00FFE8,
    /** [AVATAR_DEFAULT], a preset 1..[AVATAR_PRESETS] or [AVATAR_CUSTOM]. */
    val avatar: Int = AVATAR_DEFAULT,
    val etaTimeLeft: Boolean = true,
    val clock12h: Boolean = false,
    val imperial: Boolean = false,
    val dialOuterTrip: Boolean = false,
    /** Rider mesh (docs/reference/rider_mesh_v1.md): friends in range drawn on the compass. */
    val friendsOnCompass: Boolean = true,
    /** Friend marker: their character (false) or a dot in their colour (true). */
    val friendMarkerDot: Boolean = false,
    /** Rider mesh radio; off makes the terminal silent to other riders. */
    val meshEnabled: Boolean = true,
) {
    companion object {
        const val AVATAR_DEFAULT = 0
        /** Presets in order: crab, owl, dino, octopus, chick, slime, bear, dragon, robot. */
        const val AVATAR_PRESETS = 9
        const val AVATAR_CUSTOM = 0xFF
    }
}

object SettingsTlv {
    const val SCHEMA = 0x01
    const val TAG_ACCENT = 0x01
    const val TAG_AVATAR = 0x02
    const val TAG_ETA_FORMAT = 0x03
    const val TAG_CLOCK_12H = 0x04
    const val TAG_UNITS = 0x05
    const val TAG_DIAL_OUTER = 0x06
    const val TAG_FRIENDS_ON_COMPASS = 0x07
    const val TAG_FRIEND_MARKER = 0x08
    const val TAG_MESH_ENABLED = 0x09

    /** Largest single write: payload of the BLE-spec default 23-byte ATT MTU. */
    const val MAX_WRITE_BYTES = 20

    val DEFAULTS_FIXTURE: ByteArray = bytes(
        0x01, 0x01, 0x03, 0x00, 0xFF, 0xE8, 0x02, 0x01, 0x00, 0x03, 0x01, 0x01,
        0x04, 0x01, 0x00, 0x05, 0x01, 0x00, 0x06, 0x01, 0x00, 0x07, 0x01, 0x01,
        0x08, 0x01, 0x00, 0x09, 0x01, 0x01,
    )

    private fun records(s: TerminalSettings): List<ByteArray> = listOf(
        bytes(TAG_ACCENT, 3, (s.accentRgb shr 16) and 0xFF, (s.accentRgb shr 8) and 0xFF, s.accentRgb and 0xFF),
        bytes(TAG_AVATAR, 1, s.avatar),
        bytes(TAG_ETA_FORMAT, 1, s.etaTimeLeft.bit()),
        bytes(TAG_CLOCK_12H, 1, s.clock12h.bit()),
        bytes(TAG_UNITS, 1, s.imperial.bit()),
        bytes(TAG_DIAL_OUTER, 1, s.dialOuterTrip.bit()),
        bytes(TAG_FRIENDS_ON_COMPASS, 1, s.friendsOnCompass.bit()),
        bytes(TAG_FRIEND_MARKER, 1, s.friendMarkerDot.bit()),
        bytes(TAG_MESH_ENABLED, 1, s.meshEnabled.bit()),
    )

    /** Full config, as the terminal returns it from a read. */
    fun encode(s: TerminalSettings): ByteArray =
        records(s).fold(byteArrayOf(SCHEMA.toByte())) { acc, r -> acc + r }

    /**
     * [s] as partial writes of at most [MAX_WRITE_BYTES], each a valid TLV on its own. The terminal
     * applies each one atomically, so sending them in order leaves it fully configured.
     */
    fun encodeWrites(s: TerminalSettings): List<ByteArray> {
        val writes = mutableListOf<ByteArray>()
        var cur = byteArrayOf(SCHEMA.toByte())
        for (r in records(s)) {
            if (cur.size + r.size > MAX_WRITE_BYTES) {
                writes += cur
                cur = byteArrayOf(SCHEMA.toByte())
            }
            cur += r
        }
        writes += cur
        return writes
    }

    /** Parses a settings read. Null on a schema this app doesn't know or a malformed record. */
    fun decode(buf: ByteArray, base: TerminalSettings = TerminalSettings()): TerminalSettings? {
        if (buf.isEmpty() || buf[0].u() != SCHEMA) return null
        var s = base
        var i = 1
        while (i < buf.size) {
            if (buf.size - i < 2) return null
            val tag = buf[i].u()
            val n = buf[i + 1].u()
            if (buf.size - i - 2 < n) return null
            val v = i + 2
            i += 2 + n
            if (tag == TAG_ACCENT) {
                if (n != 3) return null
                s = s.copy(accentRgb = (buf[v].u() shl 16) or (buf[v + 1].u() shl 8) or buf[v + 2].u())
                continue
            }
            if (tag !in TAG_AVATAR..TAG_MESH_ENABLED) continue // unknown: skip, like the terminal
            if (n != 1) return null
            val b = buf[v].u()
            if (tag == TAG_AVATAR) {
                if (b > TerminalSettings.AVATAR_PRESETS && b != TerminalSettings.AVATAR_CUSTOM) return null
                s = s.copy(avatar = b)
                continue
            }
            if (b > 1) return null
            val on = b == 1
            s = when (tag) {
                TAG_ETA_FORMAT -> s.copy(etaTimeLeft = on)
                TAG_CLOCK_12H -> s.copy(clock12h = on)
                TAG_UNITS -> s.copy(imperial = on)
                TAG_DIAL_OUTER -> s.copy(dialOuterTrip = on)
                TAG_FRIENDS_ON_COMPASS -> s.copy(friendsOnCompass = on)
                TAG_FRIEND_MARKER -> s.copy(friendMarkerDot = on)
                else -> s.copy(meshEnabled = on)
            }
        }
        return s
    }
}

/** Device-info characteristic (docs/reference/settings_and_avatar.md). */
data class DeviceInfo(
    val boardId: Int,
    val gfxTier: Int,
    val settingsSchema: Int,
    val avatarUpload: Boolean,
    val avatarPx: Int,
    val firmwareVersion: String,
) {
    val isS3: Boolean get() = boardId == BOARD_S3

    companion object {
        const val BOARD_C3 = 1
        const val BOARD_S3 = 2
        private const val HEADER_BYTES = 7

        fun parse(buf: ByteArray): DeviceInfo? {
            if (buf.size < HEADER_BYTES || buf[0].u() != 1) return null
            return DeviceInfo(
                boardId = buf[1].u(),
                gfxTier = buf[2].u(),
                settingsSchema = buf[3].u(),
                avatarUpload = buf[4].u() and 0x01 != 0,
                avatarPx = buf[5].u() or (buf[6].u() shl 8),
                firmwareVersion = buf.decodeToString(HEADER_BYTES, buf.size),
            )
        }
    }
}

/**
 * Colour-agnostic avatar art (docs/reference/settings_and_avatar.md): a 16x16 and an 8x8 image of 2-bit levels, 0 transparent
 * to 3 full accent, which the terminal tints with the accent colour. [bytes] is the 80-byte wire
 * and storage form.
 */
class AvatarArt private constructor(val bytes: ByteArray) {
    /** Level 0..3 at ([x], [y]) of the 16x16 image, or of the 8x8 one when [small]. */
    fun level(x: Int, y: Int, small: Boolean = false): Int {
        val (edge, base) = if (small) SMALL to LARGE * LARGE / 4 else LARGE to 0
        val i = y * edge + x
        return (bytes[base + i / 4].u() shr (6 - 2 * (i % 4))) and 3
    }

    override fun equals(other: Any?) = other is AvatarArt && bytes.contentEquals(other.bytes)
    override fun hashCode() = bytes.contentHashCode()

    companion object {
        const val LARGE = 16
        const val SMALL = 8
        const val BYTES = (LARGE * LARGE + SMALL * SMALL) / 4

        fun fromBytes(buf: ByteArray): AvatarArt? = if (buf.size == BYTES) AvatarArt(buf.copyOf()) else null

        /** [large] is 16x16 and [small] 8x8 levels, row-major, each 0..3. */
        fun fromLevels(large: IntArray, small: IntArray): AvatarArt {
            require(large.size == LARGE * LARGE && small.size == SMALL * SMALL) { "need 16x16 and 8x8" }
            require((large + small).all { it in 0..3 }) { "levels are 0..3" }
            val all = large + small
            return AvatarArt(ByteArray(BYTES) { b ->
                ((all[4 * b] shl 6) or (all[4 * b + 1] shl 4) or (all[4 * b + 2] shl 2) or all[4 * b + 3]).toByte()
            })
        }
    }
}

private fun bytes(vararg v: Int) = ByteArray(v.size) { v[it].toByte() }
private fun Byte.u() = toInt() and 0xFF
private fun Boolean.bit() = if (this) 1 else 0
