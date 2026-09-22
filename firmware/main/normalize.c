#include "normalize.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

// Lowercase copy of a bounded C string, ASCII only.
static void to_lower(const char *src, char *dst, size_t len) {
    size_t i = 0;
    for (; i < len - 1 && src[i] != '\0'; i++) {
        dst[i] = (char)tolower((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

static bool contains(const char *haystack, const char *needle) {
    return strstr(haystack, needle) != NULL;
}

// English keyword matching against the raw maneuver text. Brittle across locales/phrasings; this
// is the terminal-side single source of truth. Expand as on-device captures reveal new phrasings.
static nav_icon_t derive_maneuver(const char *text) {
    char lower[RAW_TITLE_STR_LEN];
    to_lower(text, lower, sizeof(lower));

    if (contains(lower, "u-turn") || contains(lower, "u turn")) return NAV_ICON_U_TURN;
    if (contains(lower, "roundabout")) return NAV_ICON_ROUNDABOUT;
    if (contains(lower, "arrive") || contains(lower, "destination") || contains(lower, "reached")) return NAV_ICON_ARRIVED;
    if (contains(lower, "sharp right")) return NAV_ICON_SHARP_RIGHT;
    if (contains(lower, "sharp left")) return NAV_ICON_SHARP_LEFT;
    if (contains(lower, "slight right") || contains(lower, "keep right")) return NAV_ICON_SLIGHT_RIGHT;
    if (contains(lower, "slight left") || contains(lower, "keep left")) return NAV_ICON_SLIGHT_LEFT;
    if (contains(lower, "right")) return NAV_ICON_TURN_RIGHT;
    if (contains(lower, "left")) return NAV_ICON_TURN_LEFT;
    if (contains(lower, "straight") || contains(lower, "continue")) return NAV_ICON_STRAIGHT;
    return NAV_ICON_UNKNOWN;
}

// Buckets a maneuver-arrow rotation angle (degrees, 0 = up/straight, clockwise positive) into the
// normalized maneuver set. This is geometry, not language, so it works for any title phrasing —
// preferred over derive_maneuver() whenever the phone was able to extract an angle (see
// icon_rotation_deg KDoc in packet.h / RawNotifPacket.kt). Thresholds are a first approximation,
// not yet validated against real on-road turns/roundabouts/u-turns beyond the straight-ahead case.
static nav_icon_t derive_maneuver_from_angle(int16_t angle_deg) {
    int a = ((int)angle_deg) % 360;
    if (a < 0) a += 360;

    if (a <= 20 || a >= 340) return NAV_ICON_STRAIGHT;
    if (a <= 180) {
        // Right side (clockwise from straight-ahead).
        if (a <= 45) return NAV_ICON_SLIGHT_RIGHT;
        if (a <= 135) return NAV_ICON_TURN_RIGHT;
        if (a <= 170) return NAV_ICON_SHARP_RIGHT;
        return NAV_ICON_U_TURN;
    }
    // Left side (counter-clockwise from straight-ahead).
    if (a >= 315) return NAV_ICON_SLIGHT_LEFT;
    if (a >= 225) return NAV_ICON_TURN_LEFT;
    if (a >= 190) return NAV_ICON_SHARP_LEFT;
    return NAV_ICON_U_TURN;
}

// Extracts the street name from "...onto/on <street>" phrasing. Returns 0 on no match.
// Roundabout/arrive/reroute text has no separable street — empty is correct, not fabricated.
static size_t extract_street(const char *text, char *out, size_t out_len) {
    char lower[RAW_TITLE_STR_LEN];
    to_lower(text, lower, sizeof(lower));

    const char *sep = strstr(lower, "onto ");
    if (sep == NULL) sep = strstr(lower, " on ");
    if (sep == NULL) return 0;

    // Find the start of the street in the ORIGINAL (case-preserved) text at the same offset.
    const char *street = text + (sep - lower) + strlen("onto ");
    size_t n = 0;
    while (n < out_len - 1 && street[n] != '\0' && street[n] != '\r' && street[n] != '\n') {
        out[n] = street[n];
        n++;
    }
    out[n] = '\0';
    return n;
}

// Parses a distance in metres from a plain number string ("250") or text-embedded number.
// Returns -1 on no parse.
static int32_t parse_distance_metres(const char *s) {
    if (s == NULL || s[0] == '\0') return -1;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s) return -1;
    if (v < 0) return -1;
    return (int32_t)v;
}

void normalize_packet(const raw_notif_t *raw, nav_model_t *out) {
    memset(out, 0, sizeof(*out));
    out->icon_type = NAV_ICON_UNKNOWN;
    out->distance_meters = -1;
    out->remaining_meters = -1;
    out->speed_kmh_x10 = NAV_U16_UNKNOWN;
    out->heading_deg = NAV_U16_UNKNOWN;
    out->battery_percent = 0xFF;
    out->sequence = raw->sequence;

    // Prefer the icon-rotation angle (language-independent) over title-text keyword matching,
    // except for arrival: "arrive"/"destination" text is unambiguous and the arrival icon isn't a
    // rotated arrow, so it doesn't map onto the angle bucketing below.
    nav_icon_t text_icon = derive_maneuver(raw->title_str);
    if (text_icon == NAV_ICON_ARRIVED) {
        out->icon_type = NAV_ICON_ARRIVED;
    } else if (raw->icon_rotation_deg != RAW_I16_UNKNOWN) {
        out->icon_type = derive_maneuver_from_angle(raw->icon_rotation_deg);
    } else {
        out->icon_type = text_icon;
    }

    // Distance: prefer the clean numeric field (Android shortCriticalText), else scan the title.
    out->distance_meters = parse_distance_metres(raw->distance_str);
    if (out->distance_meters < 0) {
        out->distance_meters = parse_distance_metres(raw->title_str);
    }

    // Remaining trip distance from progress/progressMax, when both are present and ordered.
    if (raw->progress >= 0 && raw->progress_max > 0 && raw->progress <= raw->progress_max) {
        out->remaining_meters = raw->progress_max - raw->progress;
    }

    // Telemetry passthrough with sentinel preservation.
    out->speed_kmh_x10 = raw->speed_kmh_x10;
    out->heading_deg = raw->heading_deg;
    out->battery_percent = raw->battery_percent;
    out->gnss_fix_valid = raw->gnss_fix_valid;

    if (extract_street(raw->title_str, out->street_name, sizeof(out->street_name)) == 0) {
        out->street_name[0] = '\0';
    }
    strncpy(out->eta, raw->eta_str, sizeof(out->eta) - 1);
    out->eta[sizeof(out->eta) - 1] = '\0';
}
