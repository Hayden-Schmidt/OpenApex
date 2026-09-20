#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../main/packet.h"
#include "../main/nav_model.h"
#include "../main/normalize.h"

// Builds a minimal valid v1 packet in the same layout RawNotifPacket.kt emits.
static void build_packet(uint8_t *p, const char *title, const char *eta, const char *dist,
                         int progress, int progress_max, uint16_t speed_x10, uint16_t heading,
                         uint8_t battery, bool fix_valid) {
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

static void test_normalize_roundabout_and_sharp(void) {
    uint8_t p[RAW_NOTIF_PACKET_SIZE];

    build_packet(p, "At the roundabout take the 2nd exit", NULL, NULL, -1, -1, RAW_U16_UNKNOWN, RAW_U16_UNKNOWN, RAW_U8_UNKNOWN, false);
    raw_notif_t raw;
    packet_decode(p, sizeof(p), &raw);
    nav_model_t m;
    normalize_packet(&raw, &m);
    assert(m.icon_type == NAV_ICON_ROUNDABOUT);

    build_packet(p, "Sharp left turn ahead", NULL, NULL, -1, -1, RAW_U16_UNKNOWN, RAW_U16_UNKNOWN, RAW_U8_UNKNOWN, false);
    packet_decode(p, sizeof(p), &raw);
    normalize_packet(&raw, &m);
    assert(m.icon_type == NAV_ICON_SHARP_LEFT);
}

int main(void) {
    test_decode_rejects_malformed();
    test_normalize_turn_right();
    test_normalize_arrived();
    test_normalize_unknown_uses_sentinels();
    test_normalize_roundabout_and_sharp();
    puts("packet + normalize tests passed");
    return 0;
}
