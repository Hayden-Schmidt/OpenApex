#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// v2 raw-notification relay packet — the byte layout the Android relay publishes and the
// terminal decodes. See the Android RawNotifPacket.kt KDoc for the authoritative field table.
// v2 adds raw phone accelerometer/gyroscope samples (offsets 131-142) alongside the v1 fields;
// these are diagnostic/future-use passthrough, not part of the normalized nav/countdown model.
// v2 also adds icon_rotation_deg (offset 143): the maneuver arrow's rotation angle, extracted on
// the phone from the notification's icon bitmap (geometry only — no semantic classification, kept
// on the terminal normalizer per docs/OpenApex_SPEC.md §2.4). 0 = up/straight, clockwise positive.
// It is a rough estimate and its handedness is mirrored relative to the displayed arrow, so the
// normalizer treats it as a FALLBACK behind title-text parsing (see normalize.c) — except for
// roundabout exit direction, which the title text never states.
//
// v3 adds the raw heading-fusion inputs the terminal-side filter needs (see
// docs/Heading_Sensor_Fusion_Plan.md): bearing_accuracy_deg_x10 (offset 145), yaw_deg (offset
// 147) and yaw_rate_dps_x10 (offset 149). heading_deg (offset 8) is unchanged on the wire — it was
// always the raw GPS course; what changes is the terminal no longer treats it as the final display
// heading, it's one input to heading_fusion.c alongside these three.
//
// v4 adds the phone's wall clock (epoch_s at 152, tz_offset_min at 156) -- the C3 has no RTC -- and
// Google Maps' progress-bar segments (count at 158, 8x {u16 end_permille, u8 r, g, b} from 159):
// the traffic colouring along the whole route, raw RGB, classified by normalize.c.
#define RAW_NOTIF_PACKET_SIZE 200U
#define RAW_NOTIF_VERSION 4U
#define RAW_MAX_SEGMENTS 8U

// Field buffer sizes.
#define RAW_DIST_STR_LEN 16
#define RAW_ETA_STR_LEN 32
#define RAW_TITLE_STR_LEN 64

// "Unknown" sentinels: max representable value, never a fabricated zero.
#define RAW_U8_UNKNOWN 0xFFU
#define RAW_U16_UNKNOWN 0xFFFFU
#define RAW_U32_UNKNOWN 0xFFFFFFFFU
#define RAW_I16_UNKNOWN 0x7FFF

typedef struct {
    uint32_t sequence;
    bool gnss_fix_valid;
    uint16_t speed_kmh_x10;    // RAW_U16_UNKNOWN = unknown
    uint16_t heading_deg;      // RAW_U16_UNKNOWN = unknown
    uint8_t battery_percent;   // RAW_U8_UNKNOWN = unknown
    int32_t progress;          // -1 = unknown
    int32_t progress_max;      // -1 = unknown
    char distance_str[RAW_DIST_STR_LEN];
    char eta_str[RAW_ETA_STR_LEN];
    char title_str[RAW_TITLE_STR_LEN];
    // Raw phone motion samples, milli-units. RAW_I16_UNKNOWN = no reading. Diagnostic/future-use
    // passthrough only — never fed into the countdown/navigation model (SPEC normalization rule).
    int16_t accel_mg[3];   // x, y, z accelerometer, milli-g
    int16_t gyro_mdps[3];  // x, y, z gyroscope, milli-degrees/second
    // Maneuver arrow rotation angle extracted from the notification icon bitmap, degrees,
    // 0 = up/straight, clockwise positive. RAW_I16_UNKNOWN = not extracted/unavailable.
    int16_t icon_rotation_deg;
    // Raw heading-fusion inputs (v3), never fused on the phone — see
    // docs/Heading_Sensor_Fusion_Plan.md. heading_deg above is the raw GPS course; these are its
    // reliability and the independent phone-yaw witness that heading_fusion.c blends it with.
    uint16_t bearing_accuracy_deg_x10; // RAW_U16_UNKNOWN = unavailable (pre-API26 or no fix)
    uint16_t yaw_deg;                  // 0-359, RAW_U16_UNKNOWN = no rotation-vector reading
    int16_t yaw_rate_dps_x10;          // gyro Z, RAW_I16_UNKNOWN = no gyro reading
    // Phone wall clock (v4). epoch_s is UTC seconds, RAW_U32_UNKNOWN = unknown; local time is
    // epoch_s + tz_offset_min * 60, RAW_I16_UNKNOWN = unknown offset.
    uint32_t epoch_s;
    int16_t tz_offset_min;
    // Maps progress-bar segments (v4), route order. end_permille is cumulative over the whole
    // route; a segment starts where the previous one ended (the first at 0).
    uint8_t segment_count;
    struct {
        uint16_t end_permille;
        uint8_t r, g, b;
    } segments[RAW_MAX_SEGMENTS];
} raw_notif_t;

// Decodes a raw packet into out. Returns false on wrong version or short length (malformed
// packets must never produce a fabricated navigation value). Strings are null-terminated and
// bounded to their buffer size.
bool packet_decode(const uint8_t *data, size_t len, raw_notif_t *out);
