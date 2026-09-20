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
 * | 131    | 1    | reserved         | padding (0)                            |
 * --------------------------------------------------------------------------------------------
 * Total: 132 bytes. One BLE notification after MTU negotiation (ESP32 central requests MTU >= 185).
 */
const val RAW_NOTIF_PACKET_SIZE = 132
const val RAW_NOTIF_VERSION = 1

private const val DIST_STR_BYTES = 16
private const val ETA_STR_BYTES = 32
private const val TITLE_STR_BYTES = 64

private const val U16_UNKNOWN = 0xFFFF
private const val U8_UNKNOWN = 0xFF
private const val U32_UNKNOWN = -1 // encodes as 0xFFFFFFFF

/** Raw Maps notification extras, verbatim. Null = field absent from the notification. */
data class RawNavNotification(
    val title: String?,
    val etaText: String?,
    val distanceText: String?,
    val progress: Int?,
    val progressMax: Int?,
)

/** Phone GNSS + battery telemetry. Null speed/heading/battery = no usable reading. */
data class GnssTelemetry(
    val speedKmh: Float?,
    val headingDeg: Int?,
    val fixValid: Boolean,
    val batteryPercent: Int?,
)

fun packRawNotifPacket(sequence: Int, nav: RawNavNotification, gnss: GnssTelemetry): ByteArray {
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
    // p[131] reserved stays 0
    return p
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
