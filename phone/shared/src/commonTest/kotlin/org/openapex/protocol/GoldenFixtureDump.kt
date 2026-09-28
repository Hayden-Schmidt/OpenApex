package org.openapex.protocol

import kotlin.test.Test

/**
 * Prints the golden packets as C initialisers for firmware/test_host/packet_kotlin_fixture_test.c.
 * Not an assertion -- it is how that fixture is regenerated when the layout changes on purpose.
 * Run with: gradlew :shared:jvmTest --tests '*GoldenFixtureDump*' -i
 */
class GoldenFixtureDump {
    @Test
    fun dump() {
        val gnss = GnssTelemetry(
            speedKmh = 42f, headingDeg = 90, fixValid = true, batteryPercent = 80,
            bearingAccuracyDeg = 3f, yawDeg = null, yawRateDps = null,
            latitudeDeg = 51.5, longitudeDeg = -0.12, accuracyM = 4f,
        )
        val nav = RawNavNotification("Turn left", "10 min", "200 m", 1, 10, 270)
        emit("v5_android", packRawNotifPacket(7, nav, gnss, nowEpochMs = 1_700_000_000_000, tzOffsetMin = 60))
        emit("v6_ios", telemetryOnlyPacket(7, gnss, nowEpochMs = 1_700_000_000_000, tzOffsetMin = 60))
    }

    private fun emit(name: String, b: ByteArray) {
        println("FIXTURE $name ${b.joinToString(",") { (it.toInt() and 0xFF).toString() }}")
    }
}
