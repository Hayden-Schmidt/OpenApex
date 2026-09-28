#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../main/ancs_core.h"
#include "../main/ancs_nav.h"
#include "../main/packet.h"
#include "../main/packet_merge.h"

// The merge point between the two producers that feed one view: a wire packet from the phone app
// and, on iOS only, ANCS notification text turned into a nav-only packet on the terminal.

static void build_wire(uint8_t *p, uint8_t version, uint8_t flags, const char *title,
                       const char *dist, uint16_t speed_x10) {
    memset(p, 0, RAW_NOTIF_PACKET_SIZE);
    p[0] = version;
    p[1] = flags;
    p[2] = 7; // sequence
    p[6] = (uint8_t)(speed_x10 & 0xFF);
    p[7] = (uint8_t)(speed_x10 >> 8);
    p[8] = p[9] = 0xFF;                   // heading unknown
    p[10] = 80;                           // battery
    p[11] = p[12] = p[13] = p[14] = 0xFF; // progress unknown
    p[15] = p[16] = p[17] = p[18] = 0xFF; // progress_max unknown
    if (dist != NULL) strncpy((char *)&p[19], dist, RAW_DIST_STR_LEN - 1);
    if (title != NULL) strncpy((char *)&p[67], title, RAW_TITLE_STR_LEN - 1);
    p[143] = 0xFF; p[144] = 0x7F; // icon_rotation_deg unknown
    p[145] = p[146] = 0xFF;
    p[147] = p[148] = 0xFF;
    p[149] = p[150] = 0xFF;
}

static void ancs_nav_packet(raw_notif_t *out, const char *title, const char *dist) {
    ancs_notification_t n;
    memset(&n, 0, sizeof(n));
    n.uid = 42;
    n.category = ANCS_CATEGORY_LOCATION;
    snprintf(n.app_id, sizeof(n.app_id), "com.google.Maps");
    snprintf(n.title, sizeof(n.title), "%s", title);
    snprintf(n.subtitle, sizeof(n.subtitle), "%s", dist);
    assert(ancs_nav_to_raw(&n, 1, out));
}

// A v5 packet is both halves at once, so the merge must hand back exactly what it was given.
// This is the Android no-regression condition: the restructure changes nothing on that path.
static void test_v5_is_passthrough(void) {
    uint8_t buf[RAW_NOTIF_PACKET_SIZE];
    build_wire(buf, 5, 0x01, "Turn left onto Queen St", "200 m", 435);
    // Zeroed first so the memcmp below compares only fields: packet_decode does not clear the
    // struct's padding, and the merge's own copy came from a zeroed static.
    raw_notif_t in;
    memset(&in, 0, sizeof(in));
    assert(packet_decode(buf, sizeof(buf), &in));
    assert(in.source_groups == (RAW_GROUP_NAV | RAW_GROUP_TELEMETRY));
    assert(!in.source_is_ios);

    packet_merge_reset();
    raw_notif_t out;
    assert(packet_merge_accept(&in, 1000, &out));
    assert(memcmp(&in, &out, sizeof(in)) == 0);
}

static void test_v6_flags(void) {
    uint8_t buf[RAW_NOTIF_PACKET_SIZE];
    build_wire(buf, 6, RAW_FLAG_GNSS_FIX_VALID | RAW_FLAG_NAV_FIELDS_ABSENT | RAW_FLAG_SOURCE_IS_IOS,
               NULL, NULL, 435);
    raw_notif_t in;
    assert(packet_decode(buf, sizeof(buf), &in));
    assert(in.source_groups == RAW_GROUP_TELEMETRY);
    assert(in.source_is_ios);
    assert(in.gnss_fix_valid);

    // v6 without the absent bit still carries both halves.
    build_wire(buf, 6, RAW_FLAG_GNSS_FIX_VALID, "Turn left", "200 m", 435);
    assert(packet_decode(buf, sizeof(buf), &in));
    assert(in.source_groups == (RAW_GROUP_NAV | RAW_GROUP_TELEMETRY));
    assert(!in.source_is_ios);

    // The compatibility window is exactly [5, 6]. A v5 packet with bit1 set is an Android relay
    // that never meant anything by it, not a telemetry-only write.
    build_wire(buf, 5, RAW_FLAG_GNSS_FIX_VALID | RAW_FLAG_NAV_FIELDS_ABSENT, "Turn left", "200 m", 435);
    assert(packet_decode(buf, sizeof(buf), &in));
    assert(in.source_groups == (RAW_GROUP_NAV | RAW_GROUP_TELEMETRY));

    build_wire(buf, 4, 0x01, NULL, NULL, 0);
    assert(!packet_decode(buf, sizeof(buf), &in));
    build_wire(buf, 7, 0x01, NULL, NULL, 0);
    assert(!packet_decode(buf, sizeof(buf), &in));
}

// The iOS case the whole module exists for: nav from ANCS, telemetry from the app, both visible
// at once even though no single packet ever carried both.
static void test_ios_halves_combine(void) {
    packet_merge_reset();

    raw_notif_t nav;
    ancs_nav_packet(&nav, "Turn left onto Queen St", "200 m");
    assert(nav.source_groups == RAW_GROUP_NAV);
    assert(nav.source_is_ios);

    raw_notif_t out;
    assert(packet_merge_accept(&nav, 1000, &out));
    assert(strcmp(out.title_str, "Turn left onto Queen St") == 0);
    // ANCS knows no telemetry, and the merge has heard none either: unknown, not zero.
    assert(out.speed_kmh_x10 == RAW_U16_UNKNOWN);
    assert(out.battery_percent == RAW_U8_UNKNOWN);

    uint8_t buf[RAW_NOTIF_PACKET_SIZE];
    build_wire(buf, 6, RAW_FLAG_GNSS_FIX_VALID | RAW_FLAG_NAV_FIELDS_ABSENT | RAW_FLAG_SOURCE_IS_IOS,
               NULL, NULL, 435);
    raw_notif_t tel;
    assert(packet_decode(buf, sizeof(buf), &tel));

    assert(packet_merge_accept(&tel, 1500, &out));
    assert(out.speed_kmh_x10 == 435);
    assert(out.battery_percent == 80);
    assert(out.gnss_fix_valid);
    // The telemetry write did not end the route.
    assert(strcmp(out.title_str, "Turn left onto Queen St") == 0);
    assert(strcmp(out.distance_str, "200 m") == 0);

    // And a second ANCS update does not blank the telemetry back out.
    ancs_nav_packet(&nav, "Turn right onto King St", "80 m");
    assert(packet_merge_accept(&nav, 2000, &out));
    assert(strcmp(out.title_str, "Turn right onto King St") == 0);
    assert(out.speed_kmh_x10 == 435);
}

// Neither half is held past its producer going quiet: a frozen speed or a route that outlives
// the staleness rule is a fabricated value with a timestamp on it.
static void test_holds_expire(void) {
    packet_merge_reset();

    raw_notif_t nav;
    ancs_nav_packet(&nav, "Turn left onto Queen St", "200 m");
    raw_notif_t out;
    assert(packet_merge_accept(&nav, 1000, &out));

    uint8_t buf[RAW_NOTIF_PACKET_SIZE];
    build_wire(buf, 6, RAW_FLAG_GNSS_FIX_VALID | RAW_FLAG_NAV_FIELDS_ABSENT | RAW_FLAG_SOURCE_IS_IOS,
               NULL, NULL, 435);
    raw_notif_t tel;
    assert(packet_decode(buf, sizeof(buf), &tel));

    // Telemetry keeps flowing; nav does not. Still inside the nav hold.
    assert(packet_merge_accept(&tel, 1000 + PACKET_MERGE_NAV_HOLD_MS, &out));
    assert(out.title_str[0] != '\0');

    // Past it, the nav fields go back to their sentinels -- which is exactly what the pipeline's
    // nav gate reads to decide the terminal is idle again.
    assert(packet_merge_accept(&tel, 1001 + PACKET_MERGE_NAV_HOLD_MS, &out));
    assert(out.title_str[0] == '\0');
    assert(out.distance_str[0] == '\0');
    assert(out.progress == -1);
    assert(out.icon_rotation_deg == RAW_I16_UNKNOWN);
    assert(out.speed_kmh_x10 == 435); // telemetry untouched

    // The mirror case: the app dies, ANCS keeps going.
    packet_merge_reset();
    assert(packet_merge_accept(&tel, 1000, &out));
    ancs_nav_packet(&nav, "Turn left onto Queen St", "200 m");
    assert(packet_merge_accept(&nav, 1000 + PACKET_MERGE_TELEMETRY_HOLD_MS, &out));
    assert(out.speed_kmh_x10 == 435);
    assert(packet_merge_accept(&nav, 1001 + PACKET_MERGE_TELEMETRY_HOLD_MS, &out));
    assert(out.speed_kmh_x10 == RAW_U16_UNKNOWN);
    assert(out.battery_percent == RAW_U8_UNKNOWN);
    assert(!out.gnss_fix_valid);
    assert(out.title_str[0] != '\0'); // nav untouched
}

static void test_rejects_groupless(void) {
    packet_merge_reset();
    raw_notif_t in;
    memset(&in, 0, sizeof(in));
    in.source_groups = 0;
    raw_notif_t out;
    memset(&out, 0xAB, sizeof(out));
    raw_notif_t before = out;
    assert(!packet_merge_accept(&in, 1000, &out));
    assert(memcmp(&before, &out, sizeof(out)) == 0);
}

int main(void) {
    test_v5_is_passthrough();
    test_v6_flags();
    test_ios_halves_combine();
    test_holds_expire();
    test_rejects_groupless();
    printf("packet_merge_test: all assertions passed\n");
    return 0;
}
