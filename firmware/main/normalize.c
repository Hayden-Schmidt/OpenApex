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

    // Match against the maneuver clause only, never the street name: "Turn left onto Wright St"
    // contains "right" and used to classify as a RIGHT turn. Everything from "onto "/" on " is
    // the street (same split extract_street() uses) and is cut before any keyword matching.
    char *street_sep = strstr(lower, " onto ");
    if (street_sep == NULL) street_sep = strstr(lower, " on ");
    if (street_sep != NULL) *street_sep = '\0';

    if (contains(lower, "u-turn") || contains(lower, "u turn")) return NAV_ICON_U_TURN;
    // Direction isn't in the text (Google Maps roundabout phrasing is "take the Nth exit", not
    // left/right) -- default to straight here, refined by icon_rotation_deg in normalize_packet()
    // when available. Never reclassified away from a roundabout type; see that comment for why.
    if (contains(lower, "roundabout")) return NAV_ICON_ROUNDABOUT_STRAIGHT;
    if (contains(lower, "arrive") || contains(lower, "destination") || contains(lower, "reached")) return NAV_ICON_ARRIVED;
    if (contains(lower, "sharp right")) return NAV_ICON_SHARP_RIGHT;
    if (contains(lower, "sharp left")) return NAV_ICON_SHARP_LEFT;
    // Fork/merge/exit wording is a lane change, not a 90-degree turn -- the slight glyphs are the
    // honest rendering. Checked before the bare left/right fallbacks below so they win.
    if (contains(lower, "slight right") || contains(lower, "keep right") ||
        contains(lower, "exit right") || contains(lower, "fork right") ||
        contains(lower, "merge right") || contains(lower, "ramp on the right")) return NAV_ICON_SLIGHT_RIGHT;
    if (contains(lower, "slight left") || contains(lower, "keep left") ||
        contains(lower, "exit left") || contains(lower, "fork left") ||
        contains(lower, "merge left") || contains(lower, "ramp on the left")) return NAV_ICON_SLIGHT_LEFT;
    if (contains(lower, "right")) return NAV_ICON_TURN_RIGHT;
    if (contains(lower, "left")) return NAV_ICON_TURN_LEFT;
    if (contains(lower, "straight") || contains(lower, "continue") || contains(lower, "head ") ||
        contains(lower, "depart") || contains(lower, "merge")) return NAV_ICON_STRAIGHT;
    return NAV_ICON_UNKNOWN;
}

// Buckets a maneuver-arrow rotation angle (degrees, 0 = up/straight) into the normalized maneuver
// set. This is geometry, not language, so it survives any title phrasing — but on-road capture
// showed it is only a rough approximation of the real maneuver, so it is now the FALLBACK, used
// only when derive_maneuver() couldn't classify the title text (see normalize_packet()).
//
// Handedness: the extracted angle runs COUNTER-clockwise relative to the displayed arrow (every
// left/right came out mirrored on the first on-road run), so increasing angle means "more left"
// here, not "more right". Thresholds themselves are still a first approximation.
static nav_icon_t derive_maneuver_from_angle(int16_t angle_deg) {
    int a = ((int)angle_deg) % 360;
    if (a < 0) a += 360;

    if (a <= 20 || a >= 340) return NAV_ICON_STRAIGHT;
    if (a <= 180) {
        if (a <= 45) return NAV_ICON_SLIGHT_LEFT;
        if (a <= 135) return NAV_ICON_TURN_LEFT;
        if (a <= 170) return NAV_ICON_SHARP_LEFT;
        return NAV_ICON_U_TURN;
    }
    if (a >= 315) return NAV_ICON_SLIGHT_RIGHT;
    if (a >= 225) return NAV_ICON_TURN_RIGHT;
    if (a >= 190) return NAV_ICON_SHARP_RIGHT;
    return NAV_ICON_U_TURN;
}

// Buckets an icon-rotation angle into a roundabout exit direction. Separate from
// derive_maneuver_from_angle() -- that function has no roundabout case and exists only to bucket
// turn severity for point-to-point maneuvers, not roundabout exit direction.
static nav_icon_t derive_roundabout_direction(int16_t angle_deg) {
    int a = ((int)angle_deg) % 360;
    if (a < 0) a += 360;
    // Mirrored, like derive_maneuver_from_angle(). Roundabouts read "mostly backward" on the road,
    // which outranks the single bench measurement in
    // docs/Android16_ProgressStyle_Notification_Extras.md (a right-hand exit at 87deg) that this
    // mapping used to be built on.
    //
    // "Mostly", not "always", is the tell that the handedness is only half the problem: the exit
    // tick comes from a farthest-point search over the icon mask
    // (NavNotificationRelayService.extractIconRotationDeg), which has no way to tell the exit
    // spoke from the entry spoke or from the roundabout ring itself, so it picks the wrong feature
    // on some icons regardless of which way the result is mirrored. Fixing that needs the logged
    // (angle, displayed icon, actual roundabout) triples the drive logger now captures -- this flip
    // only corrects the systematic half.
    if (a <= 20 || a >= 340) return NAV_ICON_ROUNDABOUT_STRAIGHT;
    if (a <= 180) return NAV_ICON_ROUNDABOUT_LEFT;
    return NAV_ICON_ROUNDABOUT_RIGHT;
}

static bool is_roundabout(nav_icon_t icon) {
    return icon == NAV_ICON_ROUNDABOUT_LEFT || icon == NAV_ICON_ROUNDABOUT_RIGHT ||
           icon == NAV_ICON_ROUNDABOUT_STRAIGHT;
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

    // Title-text keyword matching is PRIMARY. The icon-rotation angle is language-independent and
    // was tried as the arbiter first, but on-road capture showed the PCA angle both misjudges
    // turn severity and reports the wrong handedness often enough that it can't outrank text that
    // literally says "Turn left". It is now only the fallback for titles derive_maneuver() can't
    // classify (non-English locales, unseen phrasings, reroute/exit wording).
    //
    // Roundabouts stay a special case in the other direction: Google Maps phrases them as "take
    // the Nth exit", so the text identifies the roundabout but never the exit direction. There
    // the angle is still the only direction source -- used to refine left/right/straight, never
    // to reclassify away from "roundabout" (derive_maneuver_from_angle has no roundabout value
    // and would silently downgrade it to a plain turn).
    nav_icon_t text_icon = derive_maneuver(raw->title_str);
    if (is_roundabout(text_icon)) {
        out->icon_type = (raw->icon_rotation_deg != RAW_I16_UNKNOWN)
                              ? derive_roundabout_direction(raw->icon_rotation_deg)
                              : text_icon;
    } else if (text_icon != NAV_ICON_UNKNOWN) {
        out->icon_type = text_icon;
    } else if (raw->icon_rotation_deg != RAW_I16_UNKNOWN) {
        out->icon_type = derive_maneuver_from_angle(raw->icon_rotation_deg);
    } else {
        out->icon_type = NAV_ICON_UNKNOWN;
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
