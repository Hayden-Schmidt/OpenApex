package org.openapex.androidauto

import java.util.zip.CRC32

/**
 * Terminal UI settings, mirroring firmware/main/settings.h. Wire format and defaults are
 * docs/OpenApex_SPEC.md §5.5; [SettingsTlv.DEFAULTS_FIXTURE] is the spec's golden fixture.
 */
data class TerminalSettings(
    val accentRgb: Int = 0x00C2FF,
    val customAvatar: Boolean = false,
    val etaTimeLeft: Boolean = true,
    val clock12h: Boolean = false,
    val imperial: Boolean = false,
    val dialOuterTrip: Boolean = false,
)

object SettingsTlv {
    const val SCHEMA = 0x01
    const val TAG_ACCENT = 0x01
    const val TAG_AVATAR = 0x02
    const val TAG_ETA_FORMAT = 0x03
    const val TAG_CLOCK_12H = 0x04
    const val TAG_UNITS = 0x05
    const val TAG_DIAL_OUTER = 0x06

    /** Largest single write: payload of the BLE-spec default 23-byte ATT MTU. */
    const val MAX_WRITE_BYTES = 20

    val DEFAULTS_FIXTURE: ByteArray = bytes(
        0x01, 0x01, 0x03, 0x00, 0xC2, 0xFF, 0x02, 0x01, 0x00, 0x03, 0x01, 0x01,
        0x04, 0x01, 0x00, 0x05, 0x01, 0x00, 0x06, 0x01, 0x00,
    )

    private fun records(s: TerminalSettings): List<ByteArray> = listOf(
        bytes(TAG_ACCENT, 3, (s.accentRgb shr 16) and 0xFF, (s.accentRgb shr 8) and 0xFF, s.accentRgb and 0xFF),
        bytes(TAG_AVATAR, 1, s.customAvatar.bit()),
        bytes(TAG_ETA_FORMAT, 1, s.etaTimeLeft.bit()),
        bytes(TAG_CLOCK_12H, 1, s.clock12h.bit()),
        bytes(TAG_UNITS, 1, s.imperial.bit()),
        bytes(TAG_DIAL_OUTER, 1, s.dialOuterTrip.bit()),
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
            if (tag !in TAG_AVATAR..TAG_DIAL_OUTER) continue // unknown: skip, like the terminal
            if (n != 1 || buf[v].u() > 1) return null
            val on = buf[v].u() == 1
            s = when (tag) {
                TAG_AVATAR -> s.copy(customAvatar = on)
                TAG_ETA_FORMAT -> s.copy(etaTimeLeft = on)
                TAG_CLOCK_12H -> s.copy(clock12h = on)
                TAG_UNITS -> s.copy(imperial = on)
                else -> s.copy(dialOuterTrip = on)
            }
        }
        return s
    }
}

/** Device-info characteristic (spec §5.5). */
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
                firmwareVersion = String(buf, HEADER_BYTES, buf.size - HEADER_BYTES, Charsets.US_ASCII),
            )
        }
    }
}

/** Frames for the avatar upload characteristic (spec §5.5), each at most 20 bytes. */
object AvatarUpload {
    private const val OP_BEGIN = 0x01
    private const val OP_DATA = 0x02
    private const val OP_COMMIT = 0x03
    private const val OP_CLEAR = 0x04
    const val DATA_CHUNK_BYTES = 17

    /** [mask] is an A8 alpha mask, [px] x [px] row-major, px from [DeviceInfo.avatarPx]. */
    fun frames(mask: ByteArray, px: Int): List<ByteArray> {
        require(mask.size == px * px) { "mask must be ${px}x$px A8" }
        val crc = CRC32().apply { update(mask) }.value
        val out = mutableListOf(
            bytes(OP_BEGIN, px and 0xFF, px shr 8, px and 0xFF, px shr 8,
                (crc and 0xFF).toInt(), ((crc shr 8) and 0xFF).toInt(),
                ((crc shr 16) and 0xFF).toInt(), ((crc shr 24) and 0xFF).toInt()),
        )
        var off = 0
        while (off < mask.size) {
            val n = minOf(DATA_CHUNK_BYTES, mask.size - off)
            out += bytes(OP_DATA, off and 0xFF, off shr 8) + mask.copyOfRange(off, off + n)
            off += n
        }
        out += bytes(OP_COMMIT)
        return out
    }

    fun clearFrame(): ByteArray = bytes(OP_CLEAR)
}

private fun bytes(vararg v: Int) = ByteArray(v.size) { v[it].toByte() }
private fun Byte.u() = toInt() and 0xFF
private fun Boolean.bit() = if (this) 1 else 0
