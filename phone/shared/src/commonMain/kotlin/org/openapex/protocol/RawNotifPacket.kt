package org.openapex.protocol

import kotlin.math.PI
import kotlin.math.roundToInt
import kotlin.math.roundToLong

/**
 * v4 raw-notification relay packet. The phone reads raw Google Maps navigation notification
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
 * | 1      | 1    | flags            | bit0 gnss_fix_valid, bit1 nav_fields_absent (v6), bit2 source_is_ios (v6) |
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
 * | 151    | 1    | reserved         | 0                                      |
 * | 152    | 4    | epoch_s          | uint32 LE, phone wall clock (UTC), 0xFFFFFFFF = unknown |
 * | 156    | 2    | tz_offset_min    | int16 LE, local = UTC + this, 0x7FFF = unknown |
 * | 158    | 1    | segment_count    | uint8, 0..8 progress-bar segments below |
 * | 159    | 40   | segments         | 8x {uint16 LE end_permille, uint8 r, g, b} |
 * | 199    | 1    | reserved         | 0                                      |
 * | 200    | 4    | lat_e7           | int32 LE, degrees x 1e7, 0x7FFFFFFF = unknown |
 * | 204    | 4    | lon_e7           | int32 LE, degrees x 1e7, 0x7FFFFFFF = unknown |
 * | 208    | 2    | h_accuracy_m     | uint16 LE, 0xFFFF = unknown            |
 * --------------------------------------------------------------------------------------------
 * Total: 210 bytes. One BLE notification, fixed-size fragmented/reassembled regardless of the
 * negotiated MTU (see BleLink's fragmentation, ESP32 side ble_link.c).
 *
 * accel_mg/gyro_mdps are raw phone motion samples for terminal-side diagnostics/future use only —
 * they are never classified or fed into the normalized navigation/countdown model (that stays the
 * ESP32 normalizer's job per docs/reference/system_overview.md).
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
 * heading_fusion.c filter needs (docs/blueprints/02_heading_sensor_fusion.md): heading_deg above stays the
 * raw GPS course exactly as it always was — the terminal no longer treats it as the final display
 * heading, it's one input to the fusion filter alongside these three.
 *
 * epoch_s/tz_offset_min (v4) are the terminal's only time source -- the C3 has no RTC. Sent on
 * every packet, navigating or not, so the idle-screen clock works without a route.
 *
 * segments (v4) are Google Maps' ProgressStyle progress-bar segments (the traffic colouring along
 * the whole route), verbatim: cumulative end position as per-mille of the summed segment lengths,
 * plus the segment's raw RGB. Turning a colour into a traffic level is the terminal's job, like
 * every other classification. Segments past the 8th are dropped; the terminal treats the rest of
 * the route as "no data".
 *
 * lat_e7/lon_e7/h_accuracy_m (v5) are the phone's fix, which the terminal shares with friends'
 * terminals over the rider mesh (docs/reference/rider_mesh_v1.md). Never used for navigation on the terminal.
 */
const val RAW_NOTIF_PACKET_SIZE = 210
const val RAW_NOTIF_VERSION = 5

/**
 * Same 210-byte layout as v5, two more flag bits. The firmware accepts either (packet.c takes
 * versions 5 through 6), so v5 stays the default and nothing on Android changes on the wire.
 */
const val RAW_NOTIF_VERSION_IOS = 6

const val RAW_FLAG_GNSS_FIX_VALID = 0x01

/**
 * v6. The nav half of this packet carries nothing: do not let it blank nav that arrived from
 * somewhere else. On iOS the nav comes over ANCS, straight to the terminal, while the app supplies
 * only telemetry -- so without this bit every telemetry packet would wipe the turn on screen.
 * packet_merge.c on the terminal is the other half of this.
 */
const val RAW_FLAG_NAV_FIELDS_ABSENT = 0x02

/** v6. Tells the terminal the app cannot be the nav source, so ANCS is not a fallback but the only one. */
const val RAW_FLAG_SOURCE_IS_IOS = 0x04
const val MAX_PROGRESS_SEGMENTS = 8

private const val DIST_STR_BYTES = 16
private const val ETA_STR_BYTES = 32
private const val TITLE_STR_BYTES = 64

private const val U16_UNKNOWN = 0xFFFF
private const val U8_UNKNOWN = 0xFF
private const val U32_UNKNOWN = -1 // encodes as 0xFFFFFFFF
private const val I16_UNKNOWN = 0x7FFF
private const val I32_UNKNOWN = 0x7FFFFFFF

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
    // v4: ProgressStyle segments in route order. Empty = none in the notification.
    val segments: List<ProgressSegment> = emptyList(),
)

/** One Notification.ProgressStyle.Segment: its length in the style's own units and ARGB colour. */
data class ProgressSegment(val length: Int, val color: Int)

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
    // v5: the fix itself, for the rider mesh. Null = no fix.
    val latitudeDeg: Double? = null,
    val longitudeDeg: Double? = null,
    val accuracyM: Float? = null,
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
    // No defaults: commonMain has no ambient clock, and it should not have one. The caller
    // owns "now" and its UTC offset, which also makes every packet in a test reproducible.
    nowEpochMs: Long?,
    tzOffsetMin: Int?,
    version: Int = RAW_NOTIF_VERSION,
    extraFlags: Int = 0,
): ByteArray {
    val p = ByteArray(RAW_NOTIF_PACKET_SIZE)
    p[0] = version.toByte()
    p[1] = ((if (gnss.fixValid) RAW_FLAG_GNSS_FIX_VALID else 0) or extraFlags).toByte()

    putU32(p, 2, sequence)
    putU16(p, 6, gnss.speedKmh.scaledOrNull(10.0)?.coerceIn(0, 0xFFFE) ?: U16_UNKNOWN)
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

    putU16(p, 145, gnss.bearingAccuracyDeg.scaledOrNull(10.0)?.coerceIn(0, 0xFFFE) ?: U16_UNKNOWN)
    putU16(p, 147, gnss.yawDeg.scaledOrNull(1.0)?.mod(360)?.coerceIn(0, 0xFFFE) ?: U16_UNKNOWN)
    putI16(p, 149, gnss.yawRateDps.scaledOrNull(10.0)?.coerceIn(-0x7FFE, 0x7FFE) ?: I16_UNKNOWN)

    putU32(p, 152, nowEpochMs?.takeIf { it >= 0 }?.let { (it / 1000L).toInt() } ?: U32_UNKNOWN)
    putI16(p, 156, tzOffsetMin?.coerceIn(-0x7FFE, 0x7FFE) ?: I16_UNKNOWN)
    putSegments(p, 158, nav.segments)

    // isFinite as well as non-null: a NaN latitude is not a fix, and half a fix is no fix --
    // writing one coordinate and a sentinel for the other would put the rider on the equator.
    val hasFix = gnss.latitudeDeg?.isFinite() == true && gnss.longitudeDeg?.isFinite() == true
    putU32(p, 200, if (hasFix) (gnss.latitudeDeg!! * 1e7).roundToLong().toInt() else I32_UNKNOWN)
    putU32(p, 204, if (hasFix) (gnss.longitudeDeg!! * 1e7).roundToLong().toInt() else I32_UNKNOWN)
    putU16(p, 208, gnss.accuracyM.scaledOrNull(1.0)?.coerceIn(0, 0xFFFE) ?: U16_UNKNOWN)
    return p
}

// Cumulative per-mille ends are computed over ALL segments, so dropping those past the cap leaves
// the kept ones at their true positions rather than stretching them over the whole route.
private fun putSegments(buf: ByteArray, offset: Int, segments: List<ProgressSegment>) {
    val valid = segments.filter { it.length > 0 }
    val total = valid.sumOf { it.length.toLong() }
    if (total <= 0L) {
        buf[offset] = 0
        return
    }
    val kept = valid.take(MAX_PROGRESS_SEGMENTS)
    buf[offset] = kept.size.toByte()
    var cumulative = 0L
    kept.forEachIndexed { i, seg ->
        cumulative += seg.length
        val at = offset + 1 + i * 5
        putU16(buf, at, ((cumulative * 1000L) / total).toInt().coerceIn(0, 1000))
        buf[at + 2] = ((seg.color shr 16) and 0xFF).toByte()
        buf[at + 3] = ((seg.color shr 8) and 0xFF).toByte()
        buf[at + 4] = (seg.color and 0xFF).toByte()
    }
}

private const val MS2_PER_G = 9.80665
// Rounds `this * scale`, or null when there is no usable reading.
//
// The null case is why this exists. Math.round(NaN) was 0, so a sensor that reported NaN -- a
// GNSS accuracy with no fix behind it, a yaw before the rotation vector settles -- used to be
// written as a real zero, which is precisely the fabricated value the wire format has sentinels
// to avoid. kotlin.math.roundToInt throws on NaN instead, so neither the old behaviour nor the
// literal translation is right: unknown has to stay unknown.
private fun Float?.scaledOrNull(scale: Double): Int? {
    val v = this ?: return null
    if (v.isNaN() || v.isInfinite()) return null
    return (v * scale).roundToLong().toInt()
}

private const val RAD_PER_S_TO_DEG_PER_S = 180.0 / PI

// Packs a 3-axis reading as round(value * scale) milli-units, or I16_UNKNOWN per axis when absent.
private fun putMotionVector(buf: ByteArray, offset: Int, values: FloatArray?, scale: Double) {
    for (axis in 0..2) {
        val raw = values?.getOrNull(axis)
        val milli = raw.scaledOrNull(scale)?.coerceIn(-0x7FFE, 0x7FFE) ?: I16_UNKNOWN
        putI16(buf, offset + axis * 2, milli)
    }
}

private fun putI16(buf: ByteArray, offset: Int, value: Int) {
    buf[offset] = (value and 0xFF).toByte()
    buf[offset + 1] = ((value shr 8) and 0xFF).toByte()
}

private fun putString(buf: ByteArray, offset: Int, max: Int, value: String?) {
    val bytes = value?.encodeToByteArray() ?: ByteArray(0)
    val n = bytes.size.coerceAtMost(max - 1)
    bytes.copyInto(buf, offset, 0, n)
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

/**
 * A packet carrying telemetry and no navigation, for iOS, where the app is only ever the telemetry
 * half: nav reaches the terminal over ANCS and the app never sees it.
 *
 * The nav fields are not merely empty, they are flagged absent. An empty maneuver means "riding,
 * not navigating" and the terminal settles on its compass view -- which is right on Android, where
 * the app would know if there were a turn, and wrong on iOS, where it would erase a turn ANCS just
 * delivered.
 */
fun telemetryOnlyPacket(
    sequence: Int,
    gnss: GnssTelemetry,
    motion: MotionTelemetry = MotionTelemetry(null, null),
    nowEpochMs: Long?,
    tzOffsetMin: Int?,
): ByteArray = packRawNotifPacket(
    sequence,
    RawNavNotification(null, null, null, null, null, null),
    gnss,
    motion,
    nowEpochMs = nowEpochMs,
    tzOffsetMin = tzOffsetMin,
    version = RAW_NOTIF_VERSION_IOS,
    extraFlags = RAW_FLAG_NAV_FIELDS_ABSENT or RAW_FLAG_SOURCE_IS_IOS,
)
