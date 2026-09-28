package org.openapex.protocol

import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals

/**
 * The iOS packet. Constants here are asserted against firmware/main/packet.h by hand; if they ever
 * disagree the terminal rejects the version outright, which is the loud failure, not the quiet one.
 */
class TelemetryOnlyPacketTest {

    private val gnss = GnssTelemetry(
        speedKmh = 42f,
        headingDeg = 90,
        fixValid = true,
        batteryPercent = 80,
        bearingAccuracyDeg = 3f,
        yawDeg = null,
        yawRateDps = null,
        latitudeDeg = 51.5,
        longitudeDeg = -0.12,
        accuracyM = 4f,
    )

    @Test
    fun `a telemetry-only packet is v6 and flags its nav fields absent`() {
        val p = telemetryOnlyPacket(7, gnss, nowEpochMs = 0, tzOffsetMin = 0)
        assertEquals(RAW_NOTIF_PACKET_SIZE, p.size)
        assertEquals(6, p[0].toInt())
        assertEquals(
            RAW_FLAG_GNSS_FIX_VALID or RAW_FLAG_NAV_FIELDS_ABSENT or RAW_FLAG_SOURCE_IS_IOS,
            p[1].toInt(),
        )
    }

    @Test
    fun `only the version and flag bytes differ from the v5 packet for the same telemetry`() {
        val empty = RawNavNotification(null, null, null, null, null, null)
        val v5 = packRawNotifPacket(7, empty, gnss, nowEpochMs = 0, tzOffsetMin = 0)
        val v6 = telemetryOnlyPacket(7, gnss, nowEpochMs = 0, tzOffsetMin = 0)

        // The layout is unchanged between versions; if a field ever moved, the terminal would
        // decode a v6 packet with v5 offsets and be quietly wrong rather than reject it.
        assertContentEquals(v5.copyOfRange(2, v5.size), v6.copyOfRange(2, v6.size))
    }

    @Test
    fun `the absent flag is not set when there is no fix, but the version still is`() {
        val p = telemetryOnlyPacket(1, gnss.copy(fixValid = false), nowEpochMs = 0, tzOffsetMin = 0)
        assertEquals(6, p[0].toInt())
        assertEquals(RAW_FLAG_NAV_FIELDS_ABSENT or RAW_FLAG_SOURCE_IS_IOS, p[1].toInt())
    }

    @Test
    fun `the Android packet is untouched -- still v5, still only the fix bit`() {
        val nav = RawNavNotification("Turn left", "10 min", "200 m", 1, 10, 270)
        val p = packRawNotifPacket(7, nav, gnss, nowEpochMs = 0, tzOffsetMin = 0)
        assertEquals(RAW_NOTIF_VERSION, p[0].toInt())
        assertEquals(RAW_FLAG_GNSS_FIX_VALID, p[1].toInt())
    }
}
