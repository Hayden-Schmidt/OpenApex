package org.openapex.androidauto

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test

class RawNotifPacketTest {

    @Test
    fun `packs full payload to exact size with all fields`() {
        val packet = packRawNotifPacket(
            sequence = 42,
            nav = RawNavNotification(
                title = "Turn right onto Main St",
                etaText = "12 min",
                distanceText = "250",
                progress = 100,
                progressMax = 1000,
            ),
            gnss = GnssTelemetry(
                speedKmh = 36.5f,
                headingDeg = 90,
                fixValid = true,
                batteryPercent = 85,
            ),
        )
        assertEquals(RAW_NOTIF_PACKET_SIZE, packet.size)
        assertEquals(RAW_NOTIF_VERSION.toByte(), packet[0])
        assertEquals(0x01, packet[1].toInt() and 0xFF) // fix valid flag

        // sequence little-endian at offset 2
        assertEquals(42, (packet[2].toInt() and 0xFF) or ((packet[3].toInt() and 0xFF) shl 8))
        // speed x10 = 365 at offset 6
        assertEquals(365, (packet[6].toInt() and 0xFF) or ((packet[7].toInt() and 0xFF) shl 8))
        // heading = 90 at offset 8
        assertEquals(90, (packet[8].toInt() and 0xFF) or ((packet[9].toInt() and 0xFF) shl 8))
        // battery = 85
        assertEquals(85, packet[10].toInt() and 0xFF)
        // title bytes at 67
        assertEquals("Turn right onto Main St", readString(packet, 67))
        // distance str at 19
        assertEquals("250", readString(packet, 19))
        // eta at 35
        assertEquals("12 min", readString(packet, 35))
    }

    @Test
    fun `motion samples pack as milli-units and default to unknown`() {
        val withMotion = packRawNotifPacket(
            sequence = 1,
            nav = RawNavNotification(title = null, etaText = null, distanceText = null, progress = null, progressMax = null),
            gnss = GnssTelemetry(speedKmh = null, headingDeg = null, fixValid = false, batteryPercent = null),
            motion = MotionTelemetry(accelMs2 = floatArrayOf(0f, 0f, 9.80665f), gyroRadS = floatArrayOf(0f, 0f, 0f)),
        )
        assertEquals(RAW_NOTIF_PACKET_SIZE, withMotion.size)
        assertEquals(1000, readI16(withMotion, 135)) // 1 g on z axis = 1000 milli-g
        assertEquals(0, readI16(withMotion, 137)) // gyro x = 0

        val withoutMotion = packRawNotifPacket(
            sequence = 1,
            nav = RawNavNotification(title = null, etaText = null, distanceText = null, progress = null, progressMax = null),
            gnss = GnssTelemetry(speedKmh = null, headingDeg = null, fixValid = false, batteryPercent = null),
        )
        assertEquals(0x7FFF, readI16(withoutMotion, 131))
        assertEquals(0x7FFF, readI16(withoutMotion, 141))
    }

    private fun readI16(buf: ByteArray, offset: Int): Int {
        val lo = buf[offset].toInt() and 0xFF
        val hi = buf[offset + 1].toInt()
        return (hi shl 8) or lo
    }

    @Test
    fun `unknown fields use max sentinels not zero`() {
        val packet = packRawNotifPacket(
            sequence = 1,
            nav = RawNavNotification(title = "Turn left", etaText = null, distanceText = null, progress = null, progressMax = null),
            gnss = GnssTelemetry(speedKmh = null, headingDeg = null, fixValid = false, batteryPercent = null),
        )
        assertEquals(0x00, packet[1].toInt() and 0xFF) // fix invalid
        assertEquals(0xFFFF, (packet[6].toInt() and 0xFF) or ((packet[7].toInt() and 0xFF) shl 8))
        assertEquals(0xFFFF, (packet[8].toInt() and 0xFF) or ((packet[9].toInt() and 0xFF) shl 8))
        assertEquals(0xFF, packet[10].toInt() and 0xFF)
        assertEquals(0xFFFFFFFF.toInt(), (packet[11].toInt() and 0xFF) or ((packet[12].toInt() and 0xFF) shl 8) or ((packet[13].toInt() and 0xFF) shl 16) or ((packet[14].toInt() and 0xFF) shl 24))
        assertEquals("", readString(packet, 19))
    }

    @Test
    fun `long title truncates and null terminates within buffer`() {
        val longTitle = "a".repeat(200)
        val packet = packRawNotifPacket(
            sequence = 1,
            nav = RawNavNotification(title = longTitle, etaText = null, distanceText = null, progress = null, progressMax = null),
            gnss = GnssTelemetry(speedKmh = null, headingDeg = null, fixValid = false, batteryPercent = null),
        )
        val title = readString(packet, 67)
        assertEquals(63, title.length) // 64-byte buffer, null terminator at last byte
        assertEquals(0.toByte(), packet[67 + 63])
    }

    @Test
    fun `heading wraps into 0-359`() {
        val packet = packRawNotifPacket(
            sequence = 1,
            nav = RawNavNotification(title = null, etaText = null, distanceText = null, progress = null, progressMax = null),
            gnss = GnssTelemetry(speedKmh = null, headingDeg = 400, fixValid = true, batteryPercent = null),
        )
        assertEquals(40, (packet[8].toInt() and 0xFF) or ((packet[9].toInt() and 0xFF) shl 8))
    }

    private fun readString(buf: ByteArray, offset: Int): String {
        var end = offset
        while (end < buf.size && buf[end].toInt() != 0) end++
        return String(buf, offset, end - offset, Charsets.UTF_8)
    }
}
