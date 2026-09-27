package org.openapex.androidauto

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.zip.CRC32

class SettingsTlvTest {

    @Test
    fun `defaults encode to the spec golden fixture`() {
        assertArrayEquals(SettingsTlv.DEFAULTS_FIXTURE, SettingsTlv.encode(TerminalSettings()))
        assertEquals(TerminalSettings(), SettingsTlv.decode(SettingsTlv.DEFAULTS_FIXTURE))
    }

    @Test
    fun `round trips non-default settings`() {
        val s = TerminalSettings(0xFF8000, true, false, true, true, true)
        assertEquals(s, SettingsTlv.decode(SettingsTlv.encode(s)))
    }

    @Test
    fun `split writes fit the default MTU and apply to the full config`() {
        val s = TerminalSettings(0x123456, true, false, true, true, true)
        val writes = SettingsTlv.encodeWrites(s)
        assertTrue(writes.all { it.size <= SettingsTlv.MAX_WRITE_BYTES && it[0].toInt() == SettingsTlv.SCHEMA })
        val applied = writes.fold(TerminalSettings()) { acc, w -> SettingsTlv.decode(w, acc)!! }
        assertEquals(s, applied)
    }

    @Test
    fun `skips unknown tags and rejects bad records`() {
        val unknown = byteArrayOf(0x01, 0x7F, 0x02, 0x00, 0x00, 0x05, 0x01, 0x01)
        assertEquals(TerminalSettings(imperial = true), SettingsTlv.decode(unknown))
        assertNull(SettingsTlv.decode(byteArrayOf(0x02)))                   // unknown schema
        assertNull(SettingsTlv.decode(byteArrayOf(0x01, 0x02, 0x01, 0x02))) // out of range
        assertNull(SettingsTlv.decode(byteArrayOf(0x01, 0x01, 0x02, 0x00, 0x00))) // accent len
        assertNull(SettingsTlv.decode(byteArrayOf(0x01, 0x05, 0x01)))       // truncated
    }

    @Test
    fun `parses device info`() {
        val buf = byteArrayOf(1, 2, 1, 1, 1, 136.toByte(), 0) + "v1.2".toByteArray()
        val info = DeviceInfo.parse(buf)!!
        assertTrue(info.isS3 && info.avatarUpload)
        assertEquals(136, info.avatarPx)
        assertEquals("v1.2", info.firmwareVersion)
        assertNull(DeviceInfo.parse(byteArrayOf(2, 1, 1, 1, 1, 70, 0)))
    }

    @Test
    fun `avatar frames carry the whole mask within 20 bytes`() {
        val px = 70
        val mask = ByteArray(px * px) { it.toByte() }
        val frames = AvatarUpload.frames(mask, px)
        assertTrue(frames.all { it.size <= 20 })
        val begin = frames.first()
        assertEquals(9, begin.size)
        val crc = (0 until 4).fold(0L) { acc, i -> acc or ((begin[5 + i].toLong() and 0xFF) shl (8 * i)) }
        assertEquals(CRC32().apply { update(mask) }.value, crc)
        val rebuilt = ByteArray(mask.size)
        frames.drop(1).dropLast(1).forEach { f ->
            val off = (f[1].toInt() and 0xFF) or ((f[2].toInt() and 0xFF) shl 8)
            System.arraycopy(f, 3, rebuilt, off, f.size - 3)
        }
        assertArrayEquals(mask, rebuilt)
        assertArrayEquals(byteArrayOf(0x03), frames.last())
    }
}
