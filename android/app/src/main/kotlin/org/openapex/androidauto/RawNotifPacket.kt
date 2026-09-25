package org.openapex.androidauto

import java.nio.charset.StandardCharsets

/**
 * v1 raw-notification relay packet. The phone reads raw Google Maps navigation notification
 * extras + phone GNSS telemetry and relays them verbatim to the ESP32 terminal. The phone does
 * NO classification here — maneuver/distance/street parsing is the ESP normalizer's job (shared
 * with the iOS/ANCS path, where raw text arrives on the terminal directly).
 *
 * All strings are null-terminated UTF-8, truncated to the buffer size. Numeric "unknown"
 * sentinels are the field's max representable value — never a fabricated zero.
 *
 * | Offset | Size | Field            | Encoding                              |
 * |--------|------|------------------|----------------------------------------|
 * | 0      | 1    | protocol version | constant `1`                           |
 * | 1      | 1    | flags            | bit0 gnss_fix_valid, rest reserved     |
 * | 2      | 4    | sequence         | uint32 LE                              |
 * | 6      | 2    | speed_kmh_x10    | uint16 LE, 0xFFFF = unknown            |
 * | 8      | 2    | heading_deg      | uint16 LE (0-359), 0xFFFF = unknown    |
 * | 10     | 1    | battery_percent  | uint8, 0xFF = unknown                  |
 * | 11     | 4    | progress         | uint32 LE, 0xFFFFFFFF = unknown        |
 * | 15     | 4    | progress_max     | uint32 LE, 0xFFFFFFFF = unknown        |
 * | 19     | 16   | distance_str     | android.shortCriticalText              |
 * | 35     | 32   | eta_str          | android.subText                        |
 * | 67     | 64   | title_str        | android.title (maneuver text)          |
 * | 131    | 6    | accel_mg         | 3x int16 LE, milli-g, 0x7FFF = unknown |
 * | 137    | 6    | gyro_mdps        | 3x int16 LE, milli-deg/s, 0x7FFF = unk |
 * | 143    | 2    | icon_rotation_deg| int16 LE, 0=up/straight, cw+, 0x7FFF=unk|
 * | 145    | 2    | bearing_accuracy_deg_x10 | uint16 LE, 0xFFFF = unknown     |
 * | 147    | 2    | yaw_deg          | uint16 LE (0-359), 0xFFFF = unknown    |
 * | 149    | 2    | yaw_rate_dps_x10 | int16 LE, 0x7FFF = unknown              |
 * --------------------------------------------------------------------------------------------
 * Total: 152 bytes. One BLE notification, fixed-size fragmented/reassembled regardless of the
 * negotiated MTU (see BleLink's fragmentation, ESP32 side ble_link.c).
 *
 * accel_mg/gyro_mdps are raw phone motion samples for terminal-side diagnostics/future use only —
 * they are never classified or fed into the normalized navigation/countdown model (that stays the
 * ESP32 normalizer's job per docs/OpenApex_SPEC.md §2.4).
 *
 * icon_rotation_deg is likewise raw geometry, not a classification: it's the angle of the
 * notification's maneuver arrow bitmap, extracted on the phone via image-moment analysis (the
 * bitmap itself is too large to relay and only exists on Android). Bucketing this angle into a
 * maneuver (turn/slight/sharp/u-turn) is the ESP32 normalizer's job, same as title-text parsing.
 * On-road capture showed the extracted angle is only a rough estimate and comes out mirrored
 * (left/right swapped) relative to the displayed arrow, so the normalizer now uses it only as a
 * fallback behind title-text parsing — except for roundabout exit direction, where the title text
 * ("take the Nth exit") never states a direction and this is the only source.
 *
 * bearing_accuracy_deg_x10/yaw_deg/yaw_rate_dps_x10 (v3) are the raw inputs the ESP32-side
 * heading_fusion.c filter needs (docs/Heading_Sensor_Fusion_Plan.md): heading_deg above stays the
 * raw GPS course exactly as it always was — the terminal no longer treats it as the final display
 * heading, it's one input to the fusion filter alongside these three.
 */
const val RAW_NOTIF_PACKET_SIZE = 152
const val RAW_NOTIF_VERSION = 3

private const val DIST_STR_BYTES = 16
private const val ETA_STR_BYTES = 32
private const val TITLE_STR_BYTES = 64

private const val U16_UNKNOWN = 0xFFFF
private const val U8_UNKNOWN = 0xFF
private const val U32_UNKNOWN = -1 // encodes as 0xFFFFFFFF
private const val I16_UNKNOWN = 0x7FFF

/** Raw Maps notification extras, verbatim. Null = field absent from the notification. */
data class RawNavNotification(
    val title: String?,
    val etaText: String?,
    val distanceText: String?,
    val progress: Int?,
    val progressMax: Int?,
    // Maneuver arrow rotation angle extracted from the notification icon bitmap, degrees,
    // 0 = up/straight, clockwise positive. Null = not extracted/unavailable.
    val iconRotationDeg: Int? = null,
)

/** Phone GNSS + battery telemetry. Null speed/heading/battery = no usable reading. */
data class GnssTelemetry(
    val speedKmh: Float?,
    val headingDeg: Int?,
    val fixValid: Boolean,
    val batteryPercent: Int?,
    // v3: raw inputs for the ESP32-side heading_fusion.c filter. Null = no usable reading.
    val bearingAccuracyDeg: Float? = null,
    val yawDeg: Float? = null,
    val yawRateDps: Float? = null,
)

/**
 * Raw phone motion samples (most recent SensorManager readings), in SI units. Null = no reading
 * yet. Passed through verbatim — never classified on the phone (see class KDoc above).
 */
data class MotionTelemetry(
    val accelMs2: FloatArray?, // [x, y, z], m/s^2
    val gyroRadS: FloatArray?, // [x, y, z], rad/s
)

fun packRawNotifPacket(
    sequence: Int,
    nav: RawNavNotification,
    gnss: GnssTelemetry,
    motion: MotionTelemetry = MotionTelemetry(null, null),
): ByteArray {
    val p = ByteArray(RAW_NOTIF_PACKET_SIZE)
    p[0] = RAW_NOTIF_VERSION.toByte()
    p[1] = if (gnss.fixValid) 0x01 else 0x00

    putU32(p, 2, sequence)
    putU16(p, 6, gnss.speedKmh?.let { Math.round(it * 10f).toInt().coerceIn(0, 0xFFFE) } ?: U16_UNKNOWN)
    putU16(p, 8, gnss.headingDeg?.let { it.mod(360).coerceIn(0, 0xFFFE) } ?: U16_UNKNOWN)
    p[10] = gnss.batteryPercent?.coerceIn(0, 254)?.toByte() ?: U8_UNKNOWN.toByte()
    putU32(p, 11, nav.progress?.takeIf { it >= 0 } ?: U32_UNKNOWN)
    putU32(p, 15, nav.progressMax?.takeIf { it >= 0 } ?: U32_UNKNOWN)

    putString(p, 19, DIST_STR_BYTES, nav.distanceText)
    putString(p, 35, ETA_STR_BYTES, nav.etaText)
    putString(p, 67, TITLE_STR_BYTES, nav.title)

    // accel: m/s^2 -> milli-g. gyro: rad/s -> milli-degrees/s.
    putMotionVector(p, 131, motion.accelMs2, scale = 1000.0 / MS2_PER_G)
    putMotionVector(p, 137, motion.gyroRadS, scale = 1000.0 * RAD_PER_S_TO_DEG_PER_S)
    putI16(p, 143, nav.iconRotationDeg?.mod(360) ?: I16_UNKNOWN)

    putU16(p, 145, gnss.bearingAccuracyDeg?.let { Math.round(it * 10f).coerceIn(0, 0xFFFE) } ?: U16_UNKNOWN)
    putU16(p, 147, gnss.yawDeg?.let { Math.round(it).mod(360).coerceIn(0, 0xFFFE) } ?: U16_UNKNOWN)
    putI16(p, 149, gnss.yawRateDps?.let { Math.round(it * 10f).coerceIn(-0x7FFE, 0x7FFE) } ?: I16_UNKNOWN)
    return p
}

private const val MS2_PER_G = 9.80665
private const val RAD_PER_S_TO_DEG_PER_S = 180.0 / Math.PI

// Packs a 3-axis reading as round(value * scale) milli-units, or I16_UNKNOWN per axis when absent.
private fun putMotionVector(buf: ByteArray, offset: Int, values: FloatArray?, scale: Double) {
    for (axis in 0..2) {
        val raw = values?.getOrNull(axis)
        val milli = raw?.let { Math.round(it * scale).toInt().coerceIn(-0x7FFE, 0x7FFE) } ?: I16_UNKNOWN
        putI16(buf, offset + axis * 2, milli)
    }
}

private fun putI16(buf: ByteArray, offset: Int, value: Int) {
    buf[offset] = (value and 0xFF).toByte()
    buf[offset + 1] = ((value shr 8) and 0xFF).toByte()
}

private fun putString(buf: ByteArray, offset: Int, max: Int, value: String?) {
    val bytes = value?.toByteArray(StandardCharsets.UTF_8) ?: ByteArray(0)
    val n = bytes.size.coerceAtMost(max - 1)
    System.arraycopy(bytes, 0, buf, offset, n)
    // buf is zero-filled; null terminator is guaranteed at offset + n.
}

private fun putU16(buf: ByteArray, offset: Int, value: Int) {
    buf[offset] = (value and 0xFF).toByte()
    buf[offset + 1] = ((value shr 8) and 0xFF).toByte()
}

private fun putU32(buf: ByteArray, offset: Int, value: Int) {
    buf[offset] = (value and 0xFF).toByte()
    buf[offset + 1] = ((value shr 8) and 0xFF).toByte()
    buf[offset + 2] = ((value shr 16) and 0xFF).toByte()
    buf[offset + 3] = ((value shr 24) and 0xFF).toByte()
}
