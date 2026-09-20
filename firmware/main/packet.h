#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// v1 raw-notification relay packet — the byte layout the Android relay publishes and the
// terminal decodes. See the Android RawNotifPacket.kt KDoc for the authoritative field table.
#define RAW_NOTIF_PACKET_SIZE 132U
#define RAW_NOTIF_VERSION 1U

// Field buffer sizes.
#define RAW_DIST_STR_LEN 16
#define RAW_ETA_STR_LEN 32
#define RAW_TITLE_STR_LEN 64

// "Unknown" sentinels: max representable value, never a fabricated zero.
#define RAW_U8_UNKNOWN 0xFFU
#define RAW_U16_UNKNOWN 0xFFFFU
#define RAW_U32_UNKNOWN 0xFFFFFFFFU

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
} raw_notif_t;

// Decodes a raw packet into out. Returns false on wrong version or short length (malformed
// packets must never produce a fabricated navigation value). Strings are null-terminated and
// bounded to their buffer size.
bool packet_decode(const uint8_t *data, size_t len, raw_notif_t *out);
