#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../main/packet.h"
#include "../main/nav_model.h"
#include "../main/normalize.h"

// Builds a minimal valid v1 packet in the same layout RawNotifPacket.kt emits.
// icon_rotation_deg defaults to RAW_I16_UNKNOWN (see build_packet() below) unless overridden via
// build_packet_with_angle() -- most fixtures exercise the no-angle-extracted path.
static void build_packet_with_angle(uint8_t *p, const char *title, const char *eta,
                                     const char *dist, int progress, int progress_max,
                                     uint16_t speed_x10, uint16_t heading, uint8_t battery,
                                     bool fix_valid, int16_t icon_rotation_deg) {
    memset(p, 0, RAW_NOTIF_PACKET_SIZE);
    p[0] = RAW_NOTIF_VERSION;
    p[1] = fix_valid ? 0x01 : 0x00;
    p[2] = 7; // sequence = 7
    p[6] = (uint8_t)(speed_x10 & 0xFF);
    p[7] = (uint8_t)(speed_x10 >> 8);
    p[8] = (uint8_t)(heading & 0xFF);
    p[9] = (uint8_t)(heading >> 8);
    p[10] = battery;
    if (progress >= 0) {
        p[11] = (uint8_t)(progress & 0xFF);
        p[12] = (uint8_t)((progress >> 8) & 0xFF);
        p[13] = (uint8_t)((progress >> 16) & 0xFF);
        p[14] = (uint8_t)((progress >> 24) & 0xFF);
    } else {
        p[11] = p[12] = p[13] = p[14] = 0xFF;
    }
    if (progress_max >= 0) {
        p[15] = (uint8_t)(progress_max & 0xFF);
        p[16] = (uint8_t)((progress_max >> 8) & 0xFF);
        p[17] = (uint8_t)((progress_max >> 16) & 0xFF);
        p[18] = (uint8_t)((progress_max >> 24) & 0xFF);
    } else {
        p[15] = p[16] = p[17] = p[18] = 0xFF;
    }
    if (dist != NULL) strncpy((char *)&p[19], dist, RAW_DIST_STR_LEN - 1);
    if (eta != NULL) strncpy((char *)&p[35], eta, RAW_ETA_STR_LEN - 1);
    if (title != NULL) strncpy((char *)&p[67], title, RAW_TITLE_STR_LEN - 1);
    // Motion fields default to the unknown sentinel (0x7FFF) in every fixture built here.
    for (int i = 0; i < 6; i++) {
        p[131 + i * 2] = 0xFF;
        p[131 + i * 2 + 1] = 0x7F;
    }
    p[143] = (uint8_t)(icon_rotation_deg & 0xFF);
    p[144] = (uint8_t)(((uint16_t)icon_rotation_deg >> 8) & 0xFF);
    // v3 heading-fusion inputs default to unknown in every fixture built here -- none of these
    // tests exercise heading_fusion, only normalize_packet(), which never reads these fields.
    p[145] = p[146] = 0xFF; // bearing_accuracy_deg_x10
    p[147] = p[148] = 0xFF; // yaw_deg
    p[149] = p[150] = 0xFF; // yaw_rate_dps_x10 (0x7FFF == RAW_I16_UNKNOWN)
}

// Same as build_packet_with_angle(), but with icon_rotation_deg left at RAW_I16_UNKNOWN --
// i.e. the phone didn't extract an angle from the notification icon. Used by every fixture that
// only wants to exercise the text classifier.
static void build_packet(uint8_t *p, const char *title, const char *eta, const char *dist,
                         int progress, int progress_max, uint16_t speed_x10, uint16_t heading,
                         uint8_t battery, bool fix_valid) {
    build_packet_with_angle(p, title, eta, dist, progress, progress_max, speed_x10, heading,
                             battery, fix_valid, (int16_t)RAW_I16_UNKNOWN);
}

static void test_decode_rejects_malformed(void) {
    raw_notif_t out;
    uint8_t short_pkt[RAW_NOTIF_PACKET_SIZE - 1];
    memset(short_pkt, 0, sizeof(short_pkt));
    short_pkt[0] = RAW_NOTIF_VERSION;
    assert(!packet_decode(short_pkt, sizeof(short_pkt), &out));

    uint8_t bad_version[RAW_NOTIF_PACKET_SIZE];
    memset(bad_version, 0, sizeof(bad_version));
    bad_version[0] = 99;
    assert(!packet_decode(bad_version, sizeof(bad_version), &out));
}

static void test_normalize_turn_right(void) {
    uint8_t p[RAW_NOTIF_PACKET_SIZE];
    build_packet(p, "Turn right onto Main St", "12 min", "250", 100, 1000, 365, 90, 85, true);

    raw_notif_t raw;
    assert(packet_decode(p, sizeof(p), &raw));

    nav_model_t m;
    normalize_packet(&raw, &m);

    assert(m.icon_type == NAV_ICON_TURN_RIGHT);
    assert(m.distance_meters == 250);
    assert(m.remaining_meters == 900);
    assert(strcmp(m.street_name, "Main St") == 0);
    assert(strcmp(m.eta, "12 min") == 0);
    assert(m.speed_kmh_x10 == 365);
    assert(m.heading_deg == 90);
    assert(m.battery_percent == 85);
    assert(m.gnss_fix_valid);
}

static void test_normalize_arrived(void) {
    uint8_t p[RAW_NOTIF_PACKET_SIZE];
    build_packet(p, "You have arrived", NULL, NULL, -1, -1, RAW_U16_UNKNOWN, RAW_U16_UNKNOWN, RAW_U8_UNKNOWN, false);

    raw_notif_t raw;
    assert(packet_decode(p, sizeof(p), &raw));

    nav_model_t m;
    normalize_packet(&raw, &m);

    assert(m.icon_type == NAV_ICON_ARRIVED);
    assert(m.distance_meters == -1);
    assert(m.remaining_meters == -1);
    assert(m.street_name[0] == '\0');
}

static void test_normalize_unknown_uses_sentinels(void) {
    uint8_t p[RAW_NOTIF_PACKET_SIZE];
    build_packet(p, "Continue straight", NULL, NULL, -1, -1, RAW_U16_UNKNOWN, RAW_U16_UNKNOWN, RAW_U8_UNKNOWN, false);

    raw_notif_t raw;
    assert(packet_decode(p, sizeof(p), &raw));

    nav_model_t m;
    normalize_packet(&raw, &m);

    assert(m.icon_type == NAV_ICON_STRAIGHT);
    assert(m.speed_kmh_x10 == NAV_U16_UNKNOWN);
    assert(m.heading_deg == NAV_U16_UNKNOWN);
    assert(m.battery_percent == 0xFF);
    assert(!m.gnss_fix_valid);
}

static void test_decode_motion_samples(void) {
    uint8_t p[RAW_NOTIF_PACKET_SIZE];
    build_packet(p, "Continue straight", NULL, NULL, -1, -1, RAW_U16_UNKNOWN, RAW_U16_UNKNOWN, RAW_U8_UNKNOWN, false);
    // accel z = 1000 milli-g at offset 135.
    p[135] = 0xE8;
    p[136] = 0x03;

    raw_notif_t raw;
    assert(packet_decode(p, sizeof(p), &raw));
    assert(raw.accel_mg[0] == RAW_I16_UNKNOWN);
    assert(raw.accel_mg[2] == 1000);
    assert(raw.gyro_mdps[0] == RAW_I16_UNKNOWN);
}

static void test_normalize_roundabout_and_sharp(void) {
    uint8_t p[RAW_NOTIF_PACKET_SIZE];
    raw_notif_t raw;
    nav_model_t m;

    // No angle extracted -- text alone identifies a roundabout, defaults to the straight variant.
    build_packet(p, "At the roundabout take the 2nd exit", NULL, NULL, -1, -1, RAW_U16_UNKNOWN,
                 RAW_U16_UNKNOWN, RAW_U8_UNKNOWN, false);
    packet_decode(p, sizeof(p), &raw);
    normalize_packet(&raw, &m);
    assert(m.icon_type == NAV_ICON_ROUNDABOUT_STRAIGHT);

    build_packet(p, "Sharp left turn ahead", NULL, NULL, -1, -1, RAW_U16_UNKNOWN, RAW_U16_UNKNOWN, RAW_U8_UNKNOWN, false);
    packet_decode(p, sizeof(p), &raw);
    normalize_packet(&raw, &m);
    assert(m.icon_type == NAV_ICON_SHARP_LEFT);
}

// Roundabouts: the text names the direction when it can, the glyph when it can't, and neither
// route may ever resolve to a non-roundabout icon.
//
// Angles here are the real glyph identifiers observed on the 2026-09-23 capture, not synthetic
// bearings -- see derive_maneuver_from_angle() for why an arbitrary angle is meaningless.
static void test_normalize_roundabout_direction_from_angle(void) {
    uint8_t p[RAW_NOTIF_PACKET_SIZE];
    raw_notif_t raw;
    nav_model_t m;

    // "take the Nth exit" carries no direction: the glyph decides. 135 is the first-exit glyph.
    build_packet_with_angle(p, "At the roundabout take the 1st exit", NULL, NULL, -1, -1,
                             RAW_U16_UNKNOWN, RAW_U16_UNKNOWN, RAW_U8_UNKNOWN, false, 135);
    packet_decode(p, sizeof(p), &raw);
    normalize_packet(&raw, &m);
    assert(m.icon_type == NAV_ICON_ROUNDABOUT_LEFT);

    // THE BUNNINGS REGRESSION. The text says "continue straight" in so many words; the glyph
    // (169) used to be bucketed as a left-hand exit and won, showing a left arrow for a
    // straight-through roundabout. Text must win here.
    build_packet_with_angle(p, "At the roundabout, continue straight onto Home Pl", NULL, NULL,
                             -1, -1, RAW_U16_UNKNOWN, RAW_U16_UNKNOWN, RAW_U8_UNKNOWN, false, 169);
    packet_decode(p, sizeof(p), &raw);
    normalize_packet(&raw, &m);
    assert(m.icon_type == NAV_ICON_ROUNDABOUT_STRAIGHT);

    // An unseen glyph on a roundabout still renders as a roundabout, never as a plain turn.
    build_packet_with_angle(p, "At the roundabout take the 2nd exit", NULL, NULL, -1, -1,
                             RAW_U16_UNKNOWN, RAW_U16_UNKNOWN, RAW_U8_UNKNOWN, false, 42);
    packet_decode(p, sizeof(p), &raw);
    normalize_packet(&raw, &m);
    assert(m.icon_type == NAV_ICON_ROUNDABOUT_STRAIGHT);
}

// Title text outranks the glyph wherever the text classifies at all, and the glyph table covers
// what the text cannot express.
static void test_text_outranks_angle(void) {
    uint8_t p[RAW_NOTIF_PACKET_SIZE];
    raw_notif_t raw;
    nav_model_t m;

    // Glyph 247 is the right-turn bitmap; the text says left. Text wins.
    build_packet_with_angle(p, "Turn left", NULL, NULL, -1, -1, RAW_U16_UNKNOWN,
                             RAW_U16_UNKNOWN, RAW_U8_UNKNOWN, false, 247);
    packet_decode(p, sizeof(p), &raw);
    normalize_packet(&raw, &m);
    assert(m.icon_type == NAV_ICON_TURN_LEFT);

    // Unclassifiable text (non-English phrasing) -- the glyph table is the fallback.
    build_packet_with_angle(p, "Bitte abbiegen", NULL, NULL, -1, -1, RAW_U16_UNKNOWN,
                             RAW_U16_UNKNOWN, RAW_U8_UNKNOWN, false, 113);
    packet_decode(p, sizeof(p), &raw);
    normalize_packet(&raw, &m);
    assert(m.icon_type == NAV_ICON_TURN_LEFT);

    build_packet_with_angle(p, "Bitte abbiegen", NULL, NULL, -1, -1, RAW_U16_UNKNOWN,
                             RAW_U16_UNKNOWN, RAW_U8_UNKNOWN, false, 247);
    packet_decode(p, sizeof(p), &raw);
    normalize_packet(&raw, &m);
    assert(m.icon_type == NAV_ICON_TURN_RIGHT);

    // An angle that is not a known glyph is not a maneuver. Inventing one from the old severity
    // buckets is what drew a left turn at the destination.
    build_packet_with_angle(p, "Bitte abbiegen", NULL, NULL, -1, -1, RAW_U16_UNKNOWN,
                             RAW_U16_UNKNOWN, RAW_U8_UNKNOWN, false, 57);
    packet_decode(p, sizeof(p), &raw);
    normalize_packet(&raw, &m);
    assert(m.icon_type == NAV_ICON_UNKNOWN);
}

// The destination is the one maneuver Google Maps never names: the title is the saved-place label
// ("350 m . Home"), which no keyword can match. Glyph 133 is the destination pin.
static void test_destination_glyph_is_arrived(void) {
    uint8_t p[RAW_NOTIF_PACKET_SIZE];
    raw_notif_t raw;
    nav_model_t m;

    build_packet_with_angle(p, "Home", NULL, "350", -1, -1, RAW_U16_UNKNOWN,
                             RAW_U16_UNKNOWN, RAW_U8_UNKNOWN, false, 133);
    packet_decode(p, sizeof(p), &raw);
    normalize_packet(&raw, &m);
    assert(m.icon_type == NAV_ICON_ARRIVED);
    assert(m.distance_meters == 350);

    // "Arriving" -- the stem the old "arrive" match missed entirely.
    build_packet(p, "Arriving", NULL, NULL, -1, -1, RAW_U16_UNKNOWN, RAW_U16_UNKNOWN,
                 RAW_U8_UNKNOWN, false);
    packet_decode(p, sizeof(p), &raw);
    normalize_packet(&raw, &m);
    assert(m.icon_type == NAV_ICON_ARRIVED);
}

// Motorway lane guidance is a ramp departure, not a 90-degree turn -- but "Use any lane to turn
// right" is a genuine right turn that happens to contain the same "lane" word.
static void test_lane_guidance_is_a_ramp_not_a_turn(void) {
    uint8_t p[RAW_NOTIF_PACKET_SIZE];
    raw_notif_t raw;
    nav_model_t m;

    build_packet(p, "Use the left 2 lanes to take exit 412 for Route 25", NULL, NULL, -1, -1,
                 RAW_U16_UNKNOWN, RAW_U16_UNKNOWN, RAW_U8_UNKNOWN, false);
    packet_decode(p, sizeof(p), &raw);
    normalize_packet(&raw, &m);
    assert(m.icon_type == NAV_ICON_SLIGHT_LEFT);

    build_packet(p, "Use the right lanes to take the Rte 31 ramp", NULL, NULL, -1, -1,
                 RAW_U16_UNKNOWN, RAW_U16_UNKNOWN, RAW_U8_UNKNOWN, false);
    packet_decode(p, sizeof(p), &raw);
    normalize_packet(&raw, &m);
    assert(m.icon_type == NAV_ICON_SLIGHT_RIGHT);

    build_packet(p, "Use any lane to turn right onto E Coast Rd", NULL, NULL, -1, -1,
                 RAW_U16_UNKNOWN, RAW_U16_UNKNOWN, RAW_U8_UNKNOWN, false);
    packet_decode(p, sizeof(p), &raw);
    normalize_packet(&raw, &m);
    assert(m.icon_type == NAV_ICON_TURN_RIGHT);
}

// "1.1 km" parsed as one metre through strtol(), so the terminal counted down from 1 m while the
// turn was still a kilometre out.
static void test_kilometre_distance_parses(void) {
    uint8_t p[RAW_NOTIF_PACKET_SIZE];
    raw_notif_t raw;
    nav_model_t m;

    build_packet(p, "Turn right", NULL, "1.1 km", -1, -1, RAW_U16_UNKNOWN, RAW_U16_UNKNOWN,
                 RAW_U8_UNKNOWN, false);
    packet_decode(p, sizeof(p), &raw);
    normalize_packet(&raw, &m);
    assert(m.distance_meters == 1100);

    // U+00A0 no-break space, which is what Maps actually sends between number and unit.
    build_packet(p, "Turn right", NULL, "1.0\xc2\xa0km", -1, -1, RAW_U16_UNKNOWN,
                 RAW_U16_UNKNOWN, RAW_U8_UNKNOWN, false);
    packet_decode(p, sizeof(p), &raw);
    normalize_packet(&raw, &m);
    assert(m.distance_meters == 1000);

    // Metres are unchanged.
    build_packet(p, "Turn right", NULL, "250\xc2\xa0m", -1, -1, RAW_U16_UNKNOWN,
                 RAW_U16_UNKNOWN, RAW_U8_UNKNOWN, false);
    packet_decode(p, sizeof(p), &raw);
    normalize_packet(&raw, &m);
    assert(m.distance_meters == 250);
}

// The street name must never feed the maneuver keyword match: "Wright St" contains "right".
static void test_street_name_does_not_flip_direction(void) {
    uint8_t p[RAW_NOTIF_PACKET_SIZE];
    raw_notif_t raw;
    nav_model_t m;

    build_packet(p, "Turn left onto Wright St", NULL, NULL, -1, -1, RAW_U16_UNKNOWN,
                 RAW_U16_UNKNOWN, RAW_U8_UNKNOWN, false);
    packet_decode(p, sizeof(p), &raw);
    normalize_packet(&raw, &m);
    assert(m.icon_type == NAV_ICON_TURN_LEFT);
    assert(strcmp(m.street_name, "Wright St") == 0);
}

static void test_eta_shortened(void) {
    uint8_t p[RAW_NOTIF_PACKET_SIZE];
    raw_notif_t raw;
    nav_model_t m;
    const struct {
        const char *in;
        const char *out;
    } cases[] = {
        {"Arrive 12:45", "ETA 12:45"},
        {"Arrive at 12:45 pm", "ETA 12:45 pm"},
        {"12:45 arrival", "ETA 12:45"},
        {"12 min", "12 min"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        build_packet(p, "Turn left onto Wright St", cases[i].in, NULL, -1, -1, RAW_U16_UNKNOWN,
                     RAW_U16_UNKNOWN, RAW_U8_UNKNOWN, false);
        packet_decode(p, sizeof(p), &raw);
        normalize_packet(&raw, &m);
        assert(strcmp(m.eta, cases[i].out) == 0);
    }
}

static void test_street_name_extraction_and_abbreviation(void) {
    uint8_t p[RAW_NOTIF_PACKET_SIZE];
    raw_notif_t raw;
    nav_model_t m;
    const struct {
        const char *title;
        const char *street;
    } cases[] = {
        // " on " is a shorter separator than "onto "; the street's first letter must survive.
        {"Continue on Palliser Lane", "Palliser Ln"},
        {"Turn left onto Smith Street", "Smith St"},
        {"Turn right onto Great Western Highway", "Great Western Hwy"},
        {"Turn left onto St Kilda Road North", "St Kilda Rd N"},
        // The first word is the name, never abbreviated; only whole words match.
        {"Turn right onto North Road", "North Rd"},
        {"Turn left onto Esplanade", "Esplanade"},
        {"Turn left onto Lanesborough Avenue", "Lanesborough Ave"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        build_packet(p, cases[i].title, NULL, NULL, -1, -1, RAW_U16_UNKNOWN, RAW_U16_UNKNOWN,
                     RAW_U8_UNKNOWN, false);
        packet_decode(p, sizeof(p), &raw);
        normalize_packet(&raw, &m);
        assert(strcmp(m.street_name, cases[i].street) == 0);
    }
}

/**
 * The invariant that makes tolerance matching safe.
 *
 * Glyph angles are matched within normalize_glyph_tolerance_deg() so that a Maps release nudging a
 * bitmap by a degree does not silently degrade to UNKNOWN. That is only sound while every pair of
 * entries in a table is more than 2x the tolerance apart -- at exactly 2x, an angle midway between
 * two entries is within tolerance of both, and the lookup would start answering one maneuver for
 * another. That is precisely the failure the glyph table was introduced to end, so it is checked
 * here rather than trusted to whoever adds the next entry.
 *
 * The two tables are checked independently and deliberately NOT against each other: 133 (arrived)
 * and 135 (roundabout, 1st exit) are two degrees apart but live in different families, and
 * normalize_packet() decides which family applies from the title text before either is consulted.
 */
static void test_glyph_tables_are_separable(void) {
    const int tolerance = normalize_glyph_tolerance_deg();
    size_t maneuver_count = 0, roundabout_count = 0;
    const glyph_entry_t *tables[2];
    size_t counts[2];
    tables[0] = normalize_maneuver_glyphs(&maneuver_count);
    counts[0] = maneuver_count;
    tables[1] = normalize_roundabout_glyphs(&roundabout_count);
    counts[1] = roundabout_count;

    for (int t = 0; t < 2; t++) {
        for (size_t i = 0; i < counts[t]; i++) {
            for (size_t j = i + 1; j < counts[t]; j++) {
                const int sep = normalize_angle_separation(tables[t][i].angle_deg,
                                                           tables[t][j].angle_deg);
                assert(sep > 2 * tolerance);
            }
        }
    }

    // Wrapping is handled: 359 and 1 are 2 degrees apart, not 358.
    assert(normalize_angle_separation(359, 1) == 2);
    assert(normalize_angle_separation(1, 359) == 2);
    assert(normalize_angle_separation(0, 180) == 180);
}

// A glyph that has drifted a degree or two still resolves; one that is genuinely unseen does not.
static void test_glyph_angle_tolerance(void) {
    uint8_t p[RAW_NOTIF_PACKET_SIZE];
    raw_notif_t raw;
    nav_model_t m;
    const int tolerance = normalize_glyph_tolerance_deg();

    // 247 is the observed right-turn glyph. Title text carries no maneuver, so the glyph decides.
    for (int delta = -tolerance; delta <= tolerance; delta++) {
        build_packet_with_angle(p, "Daifuku Oceania", NULL, NULL, -1, -1, RAW_U16_UNKNOWN,
                                RAW_U16_UNKNOWN, RAW_U8_UNKNOWN, false, (int16_t)(247 + delta));
        packet_decode(p, sizeof(p), &raw);
        normalize_packet(&raw, &m);
        assert(m.icon_type == NAV_ICON_TURN_RIGHT);
    }

    // Just outside the tolerance is an unseen glyph, and must NOT be guessed at.
    build_packet_with_angle(p, "Daifuku Oceania", NULL, NULL, -1, -1, RAW_U16_UNKNOWN,
                            RAW_U16_UNKNOWN, RAW_U8_UNKNOWN, false, (int16_t)(247 + tolerance + 1));
    packet_decode(p, sizeof(p), &raw);
    normalize_packet(&raw, &m);
    assert(m.icon_type == NAV_ICON_UNKNOWN);

    // Wrapping applies to the depart glyph at 0: 358 is two degrees away, not 358.
    build_packet_with_angle(p, "Daifuku Oceania", NULL, NULL, -1, -1, RAW_U16_UNKNOWN,
                            RAW_U16_UNKNOWN, RAW_U8_UNKNOWN, false, 358);
    packet_decode(p, sizeof(p), &raw);
    normalize_packet(&raw, &m);
    assert(m.icon_type == NAV_ICON_STRAIGHT);

    // A roundabout with an unseen exit glyph stays a roundabout -- never a plain turn.
    build_packet_with_angle(p, "At the roundabout, take the 2nd exit", NULL, NULL, -1, -1,
                            RAW_U16_UNKNOWN, RAW_U16_UNKNOWN, RAW_U8_UNKNOWN, false, 99);
    packet_decode(p, sizeof(p), &raw);
    normalize_packet(&raw, &m);
    assert(m.icon_type == NAV_ICON_ROUNDABOUT_STRAIGHT);
}

static void put_segment(uint8_t *p, int i, uint16_t end_permille, uint32_t rgb) {
    uint8_t *s = &p[159 + i * 5];
    s[0] = (uint8_t)(end_permille & 0xFF);
    s[1] = (uint8_t)(end_permille >> 8);
    s[2] = (uint8_t)(rgb >> 16);
    s[3] = (uint8_t)(rgb >> 8);
    s[4] = (uint8_t)rgb;
}

static void test_traffic_color_classes(void) {
    // Google's route palette: clear teal (the design's #009AA6) and blue, orange, red, dark red.
    assert(normalize_traffic_color(0x00, 0x9A, 0xA6) == NAV_TRAFFIC_FREE);
    assert(normalize_traffic_color(0x1A, 0x73, 0xE8) == NAV_TRAFFIC_FREE);
    assert(normalize_traffic_color(0x1E, 0x8E, 0x3E) == NAV_TRAFFIC_FREE);
    assert(normalize_traffic_color(0xF2, 0x99, 0x00) == NAV_TRAFFIC_SLOW);
    assert(normalize_traffic_color(0xE8, 0x71, 0x0A) == NAV_TRAFFIC_SLOW);
    assert(normalize_traffic_color(0xEA, 0x43, 0x35) == NAV_TRAFFIC_HEAVY);
    assert(normalize_traffic_color(0xD9, 0x30, 0x25) == NAV_TRAFFIC_HEAVY);
    assert(normalize_traffic_color(0xA5, 0x0E, 0x0E) == NAV_TRAFFIC_STOPPED);
    assert(normalize_traffic_color(0x8B, 0x00, 0x00) == NAV_TRAFFIC_STOPPED);
    // Neutral track and black are "no data", never "clear".
    assert(normalize_traffic_color(0x80, 0x86, 0x8B) == NAV_TRAFFIC_UNKNOWN);
    assert(normalize_traffic_color(0xE3, 0xE3, 0xE3) == NAV_TRAFFIC_UNKNOWN);
    assert(normalize_traffic_color(0x00, 0x00, 0x00) == NAV_TRAFFIC_UNKNOWN);
}

static void test_decode_v4_clock_and_traffic(void) {
    uint8_t p[RAW_NOTIF_PACKET_SIZE];
    build_packet(p, "Turn left onto Main St", NULL, "300", 250, 1000, 300, 0, 80, true);
    p[152] = 0x80; p[153] = 0xC8; p[154] = 0xB0; p[155] = 0x6A; // epoch 0x6AB0C880
    p[156] = (uint8_t)(-300 & 0xFF); p[157] = (uint8_t)((-300 >> 8) & 0xFF);
    p[158] = 3;
    put_segment(p, 0, 400, 0x009AA6);
    put_segment(p, 1, 700, 0xE8710A);
    put_segment(p, 2, 1000, 0xA50E0E);

    raw_notif_t raw;
    assert(packet_decode(p, sizeof(p), &raw));
    assert(raw.epoch_s == 0x6AB0C880U);
    assert(raw.tz_offset_min == -300);
    assert(raw.segment_count == 3);

    nav_model_t m;
    normalize_packet(&raw, &m);
    assert(m.trip_progress_permille == 250);
    assert(m.traffic_count == 3);
    assert(m.traffic[0].start_permille == 0 && m.traffic[0].end_permille == 400);
    assert(m.traffic[0].level == NAV_TRAFFIC_FREE);
    assert(m.traffic[1].start_permille == 400 && m.traffic[1].level == NAV_TRAFFIC_SLOW);
    assert(m.traffic[2].end_permille == 1000 && m.traffic[2].level == NAV_TRAFFIC_STOPPED);

    // A backwards end is a malformed table: keep what came before it, draw nothing after.
    put_segment(p, 1, 300, 0xE8710A);
    assert(packet_decode(p, sizeof(p), &raw));
    normalize_packet(&raw, &m);
    assert(m.traffic_count == 1);

    // Count past the cap never reads past the table.
    p[158] = 200;
    assert(packet_decode(p, sizeof(p), &raw));
    assert(raw.segment_count == 0);

    // No progress means unknown, not "start of trip".
    build_packet(p, "Turn left onto Main St", NULL, "300", -1, -1, 300, 0, 80, true);
    assert(packet_decode(p, sizeof(p), &raw));
    normalize_packet(&raw, &m);
    assert(m.trip_progress_permille == NAV_U16_UNKNOWN);
    assert(m.traffic_count == 0);
}

int main(void) {
    test_decode_rejects_malformed();
    test_normalize_turn_right();
    test_normalize_arrived();
    test_normalize_unknown_uses_sentinels();
    test_decode_motion_samples();
    test_normalize_roundabout_and_sharp();
    test_normalize_roundabout_direction_from_angle();
    test_text_outranks_angle();
    test_destination_glyph_is_arrived();
    test_lane_guidance_is_a_ramp_not_a_turn();
    test_kilometre_distance_parses();
    test_street_name_does_not_flip_direction();
    test_street_name_extraction_and_abbreviation();
    test_eta_shortened();
    test_glyph_tables_are_separable();
    test_glyph_angle_tolerance();
    test_traffic_color_classes();
    test_decode_v4_clock_and_traffic();
    puts("packet + normalize tests passed");
    return 0;
}
