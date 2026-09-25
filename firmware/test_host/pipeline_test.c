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
    p[143] = 0xFF; p[144] = 0x7F; // icon_rotation_deg unknown, as the phone sends with no glyph
    // v3 heading-fusion inputs: unknown in every fixture here, since these tests exercise
    // countdown/distance behavior, not heading_fusion.
    p[145] = p[146] = 0xFF; // bearing_accuracy_deg_x10
    p[147] = p[148] = 0xFF; // yaw_deg
    p[149] = p[150] = 0xFF; // yaw_rate_dps_x10 (0x7FFF == RAW_I16_UNKNOWN)
}

int main(void) {
    countdown_set_interpolation(true);
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

    // 5. Clock: empty until the phone sends time, then carried forward on the monotonic clock.
    pipeline_reset();
    memset(&view, 0, sizeof(view));
    build_packet(p, NULL, NULL, 0);
    assert(packet_decode(p, sizeof(p), &raw));
    view_state_apply_packet(&raw, 1000, &view);
    assert(view.clock[0] == '\0'); // zero-filled epoch is not 1970

    // 2026-09-25 10:15:30 UTC = 1790331330; UTC+10 = 600 min -> 20:15 local.
    uint32_t epoch = 1790331330U;
    p[152] = (uint8_t)epoch; p[153] = (uint8_t)(epoch >> 8); p[154] = (uint8_t)(epoch >> 16); p[155] = (uint8_t)(epoch >> 24);
    p[156] = 600 & 0xFF; p[157] = 600 >> 8;
    assert(packet_decode(p, sizeof(p), &raw));
    view_state_apply_packet(&raw, 2000, &view);
    assert(strcmp(view.clock, "20:15") == 0);
    view_state_tick(2000 + 30000, &view); // 10:16:00 UTC
    assert(strcmp(view.clock, "20:16") == 0);
    view_state_tick(2000 + 4 * 3600 * 1000U, &view); // wraps midnight
    assert(strcmp(view.clock, "00:15") == 0);

    // ETA: time left against the phone clock (20:15 local here), or the arrival time.
    strncpy((char *)&p[35], "Arrive 20:52", 31);
    assert(packet_decode(p, sizeof(p), &raw));
    view_state_apply_packet(&raw, 2000, &view);
    assert(strcmp(view.eta, "37 min") == 0);
    strncpy((char *)&p[35], "Arrive 0:40", 31); // crosses midnight forward
    assert(packet_decode(p, sizeof(p), &raw));
    view_state_apply_packet(&raw, 2000, &view);
    assert(strcmp(view.eta, "4 h 25 min") == 0);
    view_state_tick(2000 + 4 * 3600 * 1000U, &view); // 00:15 -> 25 min
    assert(strcmp(view.eta, "25 min") == 0);
    view_state_tick(2000 + 5 * 3600 * 1000U, &view); // 01:15: overdue reads 0, not ~23 h
    assert(strcmp(view.eta, "0 min") == 0);
    pipeline_set_eta_format(PIPELINE_ETA_ARRIVAL);
    view_state_tick(2000, &view);
    assert(strcmp(view.eta, "ETA 00:40") == 0);
    pipeline_set_eta_format(PIPELINE_ETA_TIME_LEFT);
    memset(&p[35], 0, 32);

    // 6. Odometer reaches the view: 36 km/h for 2 s = 20 m.
    build_packet(p, NULL, NULL, 360);
    assert(packet_decode(p, sizeof(p), &raw));
    view_state_apply_packet(&raw, 50000, &view);
    view_state_apply_packet(&raw, 52000, &view);
    assert(view.odometer_meters == 20);
    // Telemetry-only packets (Maps not routing) stay idle, even straight after a route ended.
    assert(view.state == VIEW_IDLE);

    puts("pipeline tests passed");
    return 0;
}
