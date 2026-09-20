#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../main/countdown.h"
#include "../main/packet.h"
#include "../main/pipeline.h"
#include "../main/view_state.h"

// Builds a valid v1 packet (see packet_normalize_test.c for the same helper).
static void build_packet(uint8_t *p, const char *title, const char *dist, uint16_t speed_x10) {
    memset(p, 0, RAW_NOTIF_PACKET_SIZE);
    p[0] = RAW_NOTIF_VERSION;
    p[1] = 0x01; // fix valid
    p[2] = 7;    // sequence
    p[6] = (uint8_t)(speed_x10 & 0xFF);
    p[7] = (uint8_t)(speed_x10 >> 8);
    p[8] = 0; // heading 0
    p[9] = 0;
    p[10] = 80; // battery
    p[11] = p[12] = p[13] = p[14] = 0xFF; // progress unknown
    p[15] = p[16] = p[17] = p[18] = 0xFF; // progress_max unknown
    if (dist != NULL) strncpy((char *)&p[19], dist, RAW_DIST_STR_LEN - 1);
    if (title != NULL) strncpy((char *)&p[67], title, RAW_TITLE_STR_LEN - 1);
}

int main(void) {
    countdown_reset();

    terminal_view_state_t view;
    memset(&view, 0, sizeof(view));
    view.state = VIEW_IDLE;

    // 1. Active maneuver with distance + speed.
    uint8_t p[RAW_NOTIF_PACKET_SIZE];
    build_packet(p, "Turn right onto Main St", "500", 360); // 36.0 km/h = 10 m/s
    raw_notif_t raw;
    assert(packet_decode(p, sizeof(p), &raw));

    view_state_apply_packet(&raw, 1000, &view);
    assert(view.state == VIEW_ACTIVE);
    assert(view.distance_meters == 500);

    // 2. Countdown advances between packets.
    view_state_tick(6000, &view); // 5 s at 10 m/s = 50 m travelled
    assert(view.distance_meters == 450);
    assert(view.state == VIEW_ACTIVE);
    assert(!view.stale);

    // 3. Stale after max hold window (10 s), and distance clamps at 0.
    view_state_tick(20000, &view);
    assert(view.state == VIEW_STALE);
    assert(view.stale);

    // 4. New maneuver sequence resets baseline cleanly.
    build_packet(p, "Turn left onto Second Ave", "100", 720); // 72 km/h = 20 m/s
    p[2] = 8; // new sequence
    assert(packet_decode(p, sizeof(p), &raw));
    view_state_apply_packet(&raw, 21000, &view);
    assert(view.state == VIEW_ACTIVE);
    assert(view.distance_meters == 100);

    puts("pipeline tests passed");
    return 0;
}
