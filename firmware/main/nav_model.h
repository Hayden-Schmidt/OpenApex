#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NAV_U16_UNKNOWN 0xFFFFU

// Stable protocol values — NOT platform enum ordinals and NOT language-specific. These are the
// single normalized maneuver set shared by the Android relay path and the iOS/ANCS path.
typedef enum {
    NAV_ICON_STRAIGHT = 0,
    NAV_ICON_TURN_LEFT,
    NAV_ICON_TURN_RIGHT,
    NAV_ICON_SLIGHT_LEFT,
    NAV_ICON_SLIGHT_RIGHT,
    NAV_ICON_SHARP_LEFT,
    NAV_ICON_SHARP_RIGHT,
    NAV_ICON_ROUNDABOUT_LEFT,
    NAV_ICON_ROUNDABOUT_RIGHT,
    NAV_ICON_ROUNDABOUT_STRAIGHT,
    NAV_ICON_U_TURN,
    NAV_ICON_ARRIVED,
    NAV_ICON_UNKNOWN,
} nav_icon_t;

#define NAV_STREET_LEN 64
#define NAV_ETA_LEN 32
#define NAV_CLOCK_LEN 8  // "HH:MM" wall clock, room for a 24h string plus terminator

// Normalized navigation model produced by the normalizer (source-agnostic). Missing fields use
// explicit sentinels; a missing field must never become a fabricated zero.
typedef struct {
    nav_icon_t icon_type;
    int32_t distance_meters;   // -1 = unknown
    int32_t remaining_meters;  // -1 = unknown
    uint16_t speed_kmh_x10;    // 0xFFFF = unknown
    uint16_t heading_deg;      // 0xFFFF = unknown
    uint8_t battery_percent;   // 0xFF = unknown
    bool gnss_fix_valid;
    uint32_t notification_age_ms; // age of source notification when decoded (0 if unknown)
    uint32_t sequence;
    char street_name[NAV_STREET_LEN];
    char eta[NAV_ETA_LEN];
} nav_model_t;
