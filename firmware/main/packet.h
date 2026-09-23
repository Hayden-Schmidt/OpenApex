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
#define RAW_NOTIF_PACKET_SIZE 146U
#define RAW_NOTIF_VERSION 2U

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
} raw_notif_t;

// Decodes a raw packet into out. Returns false on wrong version or short length (malformed
// packets must never produce a fabricated navigation value). Strings are null-terminated and
// bounded to their buffer size.
bool packet_decode(const uint8_t *data, size_t len, raw_notif_t *out);
