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
    // On the destination's street, counting down to it -- navigation has not ended yet. Appended
    // after UNKNOWN so the values above stay stable.
    NAV_ICON_DESTINATION,
} nav_icon_t;

#define NAV_STREET_LEN 64
#define NAV_ETA_LEN 32
// Upcoming-traffic runs along the remaining route, from Google Maps' notification progress bar
// (design reference/3.Turn by Turn). Eight is well past what the notification ever shows; spans
// beyond it are dropped rather than growing the view state, which is memcpy'd every frame.
#define NAV_TRAFFIC_MAX_SPANS 8

typedef enum {
    NAV_TRAFFIC_UNKNOWN = 0, // no data for this stretch -- render neutral, never as "clear"
    NAV_TRAFFIC_FREE,
    NAV_TRAFFIC_SLOW,
    NAV_TRAFFIC_HEAVY,
    NAV_TRAFFIC_STOPPED,
} nav_traffic_level_t;

// Per-mille of the WHOLE trip, not of the remaining part, so a span's numbers do not shift under it
// as the rider advances. start < end always.
typedef struct {
    uint16_t start_permille;
    uint16_t end_permille;
    uint8_t level; // nav_traffic_level_t
} nav_traffic_span_t;

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
    nav_traffic_span_t traffic[NAV_TRAFFIC_MAX_SPANS];
    uint8_t traffic_count;            // 0 = no traffic data
    uint16_t trip_progress_permille;  // NAV_U16_UNKNOWN = unknown
} nav_model_t;
