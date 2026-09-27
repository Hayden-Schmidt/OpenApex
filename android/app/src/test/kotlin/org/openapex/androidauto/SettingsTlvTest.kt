package org.openapex.androidauto

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class SettingsTlvTest {

    @Test
    fun `defaults encode to the spec golden fixture`() {
        assertArrayEquals(SettingsTlv.DEFAULTS_FIXTURE, SettingsTlv.encode(TerminalSettings()))
        assertEquals(TerminalSettings(), SettingsTlv.decode(SettingsTlv.DEFAULTS_FIXTURE))
    }

    @Test
    fun `round trips non-default settings`() {
        val s = TerminalSettings(0xFF8000, TerminalSettings.AVATAR_CUSTOM, false, true, true, true)
        assertEquals(s, SettingsTlv.decode(SettingsTlv.encode(s)))
    }

    @Test
    fun `split writes fit the default MTU and apply to the full config`() {
        val s = TerminalSettings(0x123456, 2, false, true, true, true)
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
        assertNull(SettingsTlv.decode(byteArrayOf(0x01, 0x03, 0x01, 0x02))) // out of range
        assertNull(SettingsTlv.decode(byteArrayOf(0x01, 0x02, 0x01, 0x0A))) // past the presets
        assertEquals(9, SettingsTlv.decode(byteArrayOf(0x01, 0x02, 0x01, 0x09))!!.avatar)
        assertNull(SettingsTlv.decode(byteArrayOf(0x01, 0x01, 0x02, 0x00, 0x00))) // accent len
        assertNull(SettingsTlv.decode(byteArrayOf(0x01, 0x05, 0x01)))       // truncated
    }

    @Test
    fun `parses device info`() {
        val buf = byteArrayOf(1, 2, 1, 1, 1, 128.toByte(), 0) + "v1.2".toByteArray()
        val info = DeviceInfo.parse(buf)!!
        assertTrue(info.isS3 && info.avatarUpload)
        assertEquals(128, info.avatarPx)
        assertEquals("v1.2", info.firmwareVersion)
        assertNull(DeviceInfo.parse(byteArrayOf(2, 1, 1, 1, 1, 64, 0)))
    }

    @Test
    fun `avatar art packs 2-bit levels high bits first`() {
        val large = IntArray(256) { it % 4 }
        val small = IntArray(64) { 3 - it % 4 }
        val art = AvatarArt.fromLevels(large, small)
        assertEquals(80, art.bytes.size)
        assertEquals(0x1B.toByte(), art.bytes[0])  // 0,1,2,3
        assertEquals(0xE4.toByte(), art.bytes[64]) // 3,2,1,0
        assertEquals(2, art.level(2, 0))
        assertEquals(1, art.level(2, 7, small = true))
        assertEquals(art, AvatarArt.fromBytes(art.bytes))
        assertNull(AvatarArt.fromBytes(ByteArray(79)))
    }
}
