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
    // Roundabouts: Google Maps phrases the exit as "take the Nth exit", which names no direction --
    // but it also emits "continue straight onto <street>", which names one exactly. Read that when
    // it is there; leave the rest to the glyph table in normalize_packet(). Getting this wrong is
    // what showed a left-hand roundabout for a straight-through one on the 2026-09-23 capture.
    if (contains(lower, "roundabout")) {
        if (contains(lower, "straight")) return NAV_ICON_ROUNDABOUT_STRAIGHT;
        if (contains(lower, "left")) return NAV_ICON_ROUNDABOUT_LEFT;
        if (contains(lower, "right")) return NAV_ICON_ROUNDABOUT_RIGHT;
        return NAV_ICON_UNKNOWN;  // "take the 1st exit" -- direction is only in the glyph
    }
    // "arriv" rather than "arrive": Maps says "Arriving", which the longer stem misses.
    if (contains(lower, "arriv") || contains(lower, "destination") || contains(lower, "reached")) return NAV_ICON_ARRIVED;
    if (contains(lower, "sharp right")) return NAV_ICON_SHARP_RIGHT;
    if (contains(lower, "sharp left")) return NAV_ICON_SHARP_LEFT;
    // Fork/merge/exit wording is a lane change, not a 90-degree turn -- the slight glyphs are the
    // honest rendering. Checked before the bare left/right fallbacks below so they win.
    // "slightly right" as well as "slight right": Maps uses both ("turn slightly left onto ...").
    if (contains(lower, "slight right") || contains(lower, "slightly right") ||
        contains(lower, "keep right") ||
        contains(lower, "exit right") || contains(lower, "fork right") ||
        contains(lower, "merge right") || contains(lower, "ramp on the right")) return NAV_ICON_SLIGHT_RIGHT;
    if (contains(lower, "slight left") || contains(lower, "slightly left") ||
        contains(lower, "keep left") ||
        contains(lower, "exit left") || contains(lower, "fork left") ||
        contains(lower, "merge left") || contains(lower, "ramp on the left")) return NAV_ICON_SLIGHT_LEFT;
    // Motorway lane guidance: "Use the left 2 lanes to take exit 412" and "Use the right lanes to
    // take the Rte 31 ramp" are ramp departures, not 90-degree turns, and the bare left/right
    // fallback below called both of them full turns on the 2026-09-23 capture. Gated on the
    // exit/ramp wording as well as the lane wording, so "Use any lane to turn right onto E Coast
    // Rd" -- same "lane" word, a genuine right turn -- still falls through to TURN_RIGHT.
    if (contains(lower, "lane") && (contains(lower, "exit") || contains(lower, "ramp"))) {
        if (contains(lower, "right")) return NAV_ICON_SLIGHT_RIGHT;
        if (contains(lower, "left")) return NAV_ICON_SLIGHT_LEFT;
    }
    if (contains(lower, "right")) return NAV_ICON_TURN_RIGHT;
    if (contains(lower, "left")) return NAV_ICON_TURN_LEFT;
    if (contains(lower, "straight") || contains(lower, "continue") || contains(lower, "head ") ||
        contains(lower, "depart") || contains(lower, "merge")) return NAV_ICON_STRAIGHT;
    return NAV_ICON_UNKNOWN;
}

// Maps the maneuver-arrow rotation angle onto the normalized maneuver set.
//
// This is a GLYPH LOOKUP, not geometry. The 2026-09-23 capture settled what the angle actually is:
// across 1521 packets, every maneuver class produced one exact, unvarying angle, and the
// notification's largeIcon mask fingerprint was likewise constant within the class. Google Maps
// does not rotate a single arrow -- it ships a distinct pre-rendered bitmap per maneuver, so
// NavNotificationRelayService.extractIconRotationDeg is measuring a per-glyph constant. That is
// why the old severity/handedness bucketing was never better than "mostly right": there was no
// angle to bucket.
//
// Read as an identifier the same number is exact. Each entry below is an observed
// (angle, glyph fingerprint, maneuver) triple from that capture; the fingerprint is recorded in the
// comment because the phone has it and a future packet version could carry it directly, which
// would remove the one collision noted below.
//
// MATCHED WITH TOLERANCE, not by equality. The angle is a measurement of a rendered bitmap, so it
// is only as stable as the bitmap and the measurement: a Maps release that nudges a glyph by a
// pixel, or a different display density, can move it a degree or two. Exact-match would answer
// UNKNOWN for a glyph we plainly recognise. The tolerance is deliberately far smaller than the
// gap between any two entries (the tightest is 133 -> 113, 20 degrees apart), so widening
// recognition cannot turn one maneuver into another; glyph_tables_are_separable() in
// firmware/test_host/packet_normalize_test.c enforces that invariant as the tables grow.
#define GLYPH_ANGLE_TOLERANCE_DEG 3

static const glyph_entry_t MANEUVER_GLYPHS[] = {
    // 0 is AMBIGUOUS: both the head/depart glyph (mask 0x14c1db44) and the right-hand ramp glyph
    // (0xd0c4eb44) report it. Depart is the commoner of the two and the safer default; the ramp
    // case is caught by the lane/ramp wording in derive_maneuver() before it ever reaches here.
    {0,   NAV_ICON_STRAIGHT},      // 0x14c1db44 "Head toward ..."
    {113, NAV_ICON_TURN_LEFT},     // 0x52a3927d
    {133, NAV_ICON_ARRIVED},       // 0x2060b4fa destination pin -- the title is the place name
                                   // ("Home", "Daifuku Oceania"), which no keyword can catch
    {181, NAV_ICON_STRAIGHT},      // 0xe39dc9b1 "Merge onto ..."
    {247, NAV_ICON_TURN_RIGHT},    // 0xfe8a3cb4
    {283, NAV_ICON_SHARP_RIGHT},   // 0x776c7837
    {325, NAV_ICON_SLIGHT_LEFT},   // 0x51f1bfdd left-hand exit ramp
};

static const glyph_entry_t ROUNDABOUT_GLYPHS[] = {
    {169, NAV_ICON_ROUNDABOUT_STRAIGHT},  // 0xa5c5b7f3 "continue straight onto ..."
    // 87 was reported as STRAIGHT from the 2026-09-24 afternoon ride's bearing-delta heuristic
    // (course over ground 12 s after the maneuver). That heuristic was measuring the wrong thing --
    // the 2026-09-24 evening ride replayed the identical "take the 2nd exit onto Antares Pl" glyph
    // 87 and it was a right-hand exit, matching the earlier afternoon capture of the SAME approach,
    // which also turned out right once glyph 208 fired seconds later. Two independent, unbroken
    // (no reroute/restart) runs of this exact maneuver both went 87 -> 208 (right) at the very end,
    // with 87 held the entire approach and never resolving to anything else. Retired the bearing
    // heuristic for this entry; 87 now maps straight to RIGHT rather than STRAIGHT.
    {87,  NAV_ICON_ROUNDABOUT_RIGHT},
    {208, NAV_ICON_ROUNDABOUT_RIGHT},
    // 0xcb794cdc is the "take the 1st exit" glyph. On the capture (New Zealand, left-hand traffic)
    // that exit was a left-hand one. Whether Maps ships a mirrored glyph in right-hand-traffic
    // countries is untested -- if a right-hand-drive capture ever shows 135 on a right-hand exit,
    // this entry is the thing to revisit, not the caller.
    {135, NAV_ICON_ROUNDABOUT_LEFT},
};

// Shortest angular distance between two bearings, 0..180. Needed because the table wraps: 359 and
// 1 are two degrees apart, not 358.
static int angle_separation(int a, int b) {
    int d = a - b;
    d %= 360;
    if (d < 0) d += 360;
    return d > 180 ? 360 - d : d;
}

// Nearest table entry within GLYPH_ANGLE_TOLERANCE_DEG, or `fallback` if the glyph is unseen.
// Nearest rather than first-match so that behaviour stays well-defined even if a future pair of
// entries is added closer together than the test allows.
static nav_icon_t glyph_lookup(const glyph_entry_t *table, size_t count, int16_t angle_deg,
                               nav_icon_t fallback) {
    int a = ((int)angle_deg) % 360;
    if (a < 0) a += 360;

    nav_icon_t best = fallback;
    int best_sep = GLYPH_ANGLE_TOLERANCE_DEG + 1;
    for (size_t i = 0; i < count; i++) {
        int sep = angle_separation(a, table[i].angle_deg);
        if (sep <= GLYPH_ANGLE_TOLERANCE_DEG && sep < best_sep) {
            best_sep = sep;
            best = table[i].icon;
        }
    }
    return best;
}

// Exposed for the host tests, which walk the tables to prove no two entries are close enough for
// the tolerance to confuse them. Not part of the terminal's runtime path.
const glyph_entry_t *normalize_maneuver_glyphs(size_t *count) {
    *count = sizeof(MANEUVER_GLYPHS) / sizeof(MANEUVER_GLYPHS[0]);
    return MANEUVER_GLYPHS;
}

const glyph_entry_t *normalize_roundabout_glyphs(size_t *count) {
    *count = sizeof(ROUNDABOUT_GLYPHS) / sizeof(ROUNDABOUT_GLYPHS[0]);
    return ROUNDABOUT_GLYPHS;
}

int normalize_glyph_tolerance_deg(void) {
    return GLYPH_ANGLE_TOLERANCE_DEG;
}

int normalize_angle_separation(int a, int b) {
    return angle_separation(a, b);
}

static nav_icon_t derive_maneuver_from_angle(int16_t angle_deg) {
    // An unseen glyph resolves to UNKNOWN. That is the honest answer: inventing a maneuver from an
    // angle that is not an angle is what produced a left turn at the destination on the 2026-09-23
    // ride. Unseen glyphs are recoverable from any capture without extra logging -- the RAW record
    // carries icon_rotation_deg for every packet and the MODEL record carries the icon it produced,
    // so `tools/decode_drive.py --glyphs` lists exactly which angles are not yet in these tables.
    return glyph_lookup(MANEUVER_GLYPHS,
                        sizeof(MANEUVER_GLYPHS) / sizeof(MANEUVER_GLYPHS[0]),
                        angle_deg, NAV_ICON_UNKNOWN);
}

// Roundabout exit direction from its own glyph family. Kept separate from
// derive_maneuver_from_angle() because a roundabout must never be able to resolve to a plain turn:
// the fallback here is still a roundabout, just one with an unknown exit direction.
static nav_icon_t derive_roundabout_direction(int16_t angle_deg) {
    return glyph_lookup(ROUNDABOUT_GLYPHS,
                        sizeof(ROUNDABOUT_GLYPHS) / sizeof(ROUNDABOUT_GLYPHS[0]),
                        angle_deg, NAV_ICON_ROUNDABOUT_STRAIGHT);
}

// Whether the title names a roundabout at all, independent of which exit it names. derive_maneuver()
// returns UNKNOWN for "take the 1st exit" (the text carries no direction), so the roundabout-ness
// has to be asked separately or it would be lost on exactly the phrasing that needs the glyph most.
static bool title_is_roundabout(const char *text) {
    char lower[RAW_TITLE_STR_LEN];
    to_lower(text, lower, sizeof(lower));
    return contains(lower, "roundabout");
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

// Parses a distance in metres from a plain number string ("250"), a text-embedded number, or a
// kilometre-suffixed decimal ("1.1 km"). Returns -1 on no parse.
//
// The km case is not cosmetic: strtol() stops at the decimal point, so "1.1 km" used to parse as
// ONE METRE and the terminal counted down from 1 m while the turn was still a kilometre away.
// Nineteen packets on the 2026-09-23 capture did exactly that.
static int32_t parse_distance_metres(const char *s) {
    if (s == NULL || s[0] == '\0') return -1;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s) return -1;
    if (v < 0) return -1;

    // Fractional part, if any. Kept as thousandths so the km conversion below is exact integer
    // work -- no float, and no rounding surprises on a unit whose last digit is 100 m.
    long milli = 0;
    if (*end == '.' || *end == ',') {
        const char *frac = end + 1;
        for (int i = 0; i < 3; i++) {
            milli *= 10;
            if (frac[0] >= '0' && frac[0] <= '9') {
                milli += frac[0] - '0';
                frac++;
            }
        }
        end = (char *)frac;
    }

    // Unit suffix: skip separators (including the U+00A0 no-break space Maps uses, whose UTF-8
    // bytes are 0xC2 0xA0) and look for a leading 'k'.
    while (*end == ' ' || *end == '\t' || (unsigned char)*end == 0xC2 || (unsigned char)*end == 0xA0) {
        end++;
    }
    if (*end == 'k' || *end == 'K') {
        return (int32_t)(v * 1000 + milli);
    }
    return (int32_t)v;
}

// Google Maps draws route traffic in four levels -- clear, slow, heavy, stopped -- as blue/teal,
// orange, red and dark red (the design pins clear to #009AA6). The exact shades the notification
// uses are unconfirmed and Maps restyles them between releases, so this classifies by hue and
// brightness rather than matching hex values: an unseen shade of orange still reads as slow.
// Greys and near-black are "no data" -- Maps' neutral track, never to be drawn as a clear road.
nav_traffic_level_t normalize_traffic_color(uint8_t r, uint8_t g, uint8_t b) {
    int max = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int min = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int chroma = max - min;
    if (max < 48 || chroma * 4 < max) { // too dark, or saturation < 25%
        return NAV_TRAFFIC_UNKNOWN;
    }
    int hue; // degrees, 0..359
    if (max == r) {
        hue = (60 * (g - b) / chroma + 360) % 360;
    } else if (max == g) {
        hue = 60 * (b - r) / chroma + 120;
    } else {
        hue = 60 * (r - g) / chroma + 240;
    }
    if (hue >= 80 && hue < 280) {
        return NAV_TRAFFIC_FREE; // green through teal to blue
    }
    if (hue >= 18 && hue < 80) {
        return NAV_TRAFFIC_SLOW; // orange/amber/yellow
    }
    if (hue >= 280 && hue < 330) {
        return NAV_TRAFFIC_UNKNOWN; // purple/magenta: not a traffic colour
    }
    // Red. Maps separates heavy from stopped by darkening the red, not by changing its hue.
    return max >= 0xC0 ? NAV_TRAFFIC_HEAVY : NAV_TRAFFIC_STOPPED;
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

    // Text and glyph are two independent, mutually-redundant witnesses to the same maneuver, and
    // the 2026-09-23 capture showed they agree wherever both speak. So: TEXT FIRST, because it is
    // explicit and needs no lookup table, and the GLYPH TABLE for what text cannot express.
    //
    // The two cover each other's blind spots exactly:
    //   - text has no direction for "At the roundabout, take the 1st exit"   -> glyph has it
    //   - text is just a place name at the destination ("350 m . Home")      -> glyph has it
    //   - glyph is an unseen bitmap in a non-English locale or a new release -> text has it
    //
    // A roundabout is never allowed to resolve to a plain turn: derive_roundabout_direction() has
    // no non-roundabout value, so the worst case is ROUNDABOUT_STRAIGHT, not a wrong turn arrow.
    const bool roundabout = title_is_roundabout(raw->title_str);
    nav_icon_t text_icon = derive_maneuver(raw->title_str);
    if (roundabout) {
        out->icon_type = is_roundabout(text_icon) ? text_icon
                       : (raw->icon_rotation_deg != RAW_I16_UNKNOWN)
                              ? derive_roundabout_direction(raw->icon_rotation_deg)
                              : NAV_ICON_ROUNDABOUT_STRAIGHT;
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
    out->trip_progress_permille = NAV_U16_UNKNOWN;
    if (raw->progress >= 0 && raw->progress_max > 0 && raw->progress <= raw->progress_max) {
        out->remaining_meters = raw->progress_max - raw->progress;
        out->trip_progress_permille = (uint16_t)(((int64_t)raw->progress * 1000) / raw->progress_max);
    }

    // Traffic spans from the progress-bar segments. Out-of-order or out-of-range ends mean a
    // malformed table; stop there rather than draw spans that overlap or run backwards.
    out->traffic_count = 0;
    uint16_t start = 0;
    for (uint8_t i = 0; i < raw->segment_count && i < NAV_TRAFFIC_MAX_SPANS; i++) {
        uint16_t end = raw->segments[i].end_permille;
        if (end <= start || end > 1000) {
            break;
        }
        nav_traffic_span_t *span = &out->traffic[out->traffic_count++];
        span->start_permille = start;
        span->end_permille = end;
        span->level = (uint8_t)normalize_traffic_color(raw->segments[i].r, raw->segments[i].g,
                                                       raw->segments[i].b);
        start = end;
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
