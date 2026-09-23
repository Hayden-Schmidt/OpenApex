#include "pipeline.h"

#include <string.h>

// Dev-only drive logger. This file stays host-testable: every drive_log_* call compiles to a
// static inline no-op unless OPENAPEX_DRIVE_LOG is defined, which the host tests never define.
#include "drive_log.h"
#include "normalize.h"
#include "packet.h"

// Identity of the maneuver currently being counted down, used only to tell the countdown when to
// discard its smoothed speed and start fresh.
//
// This deliberately does NOT use the packet sequence number. That increments on every packet the
// phone sends (several per second), so passing it made countdown_accept() see a "new maneuver"
// every single time and reset the speed filter on each one -- the 0.7/0.3 smoothing never ran.
// A maneuver is the same maneuver while the glyph and the street name hold steady, regardless of
// how many packets describe it.
static nav_icon_t s_last_icon = NAV_ICON_UNKNOWN;
static char s_last_street[NAV_STREET_LEN];
static uint32_t s_maneuver_id;
static bool s_have_maneuver;

static uint32_t maneuver_identity(const nav_model_t *model) {
    bool changed = !s_have_maneuver || model->icon_type != s_last_icon ||
                   strncmp(model->street_name, s_last_street, sizeof(s_last_street)) != 0;
    if (changed) {
        s_maneuver_id++;
        s_last_icon = model->icon_type;
        strncpy(s_last_street, model->street_name, sizeof(s_last_street) - 1);
        s_last_street[sizeof(s_last_street) - 1] = '\0';
        s_have_maneuver = true;
    }
    return s_maneuver_id;
}

void pipeline_reset(void) {
    s_last_icon = NAV_ICON_UNKNOWN;
    s_last_street[0] = '\0';
    s_maneuver_id = 0;
    s_have_maneuver = false;
    countdown_reset();
}

void view_state_apply_packet(const raw_notif_t *raw, uint32_t now_ms, terminal_view_state_t *out) {
    nav_model_t model;
    normalize_packet(raw, &model);
    drive_log_model(&model);

    // Distance to the NEXT MANEUVER only. remaining_meters is deliberately not used as a fallback:
    // it is progressMax - progress, the distance left in the whole trip, and substituting it made
    // the display jump from "10 m" to thousands of metres on the final update before a turn --
    // exactly when the rider needs it most. The two quantities only coincide on the last leg of a
    // journey with one maneuver left.
    int32_t distance_meters = model.distance_meters;
    bool distance_valid = distance_meters >= 0;

    countdown_input_t input;
    memset(&input, 0, sizeof(input));
    input.distance_valid = distance_valid;
    input.distance_meters = distance_valid ? (uint32_t)distance_meters : 0U;
    input.timestamp_ms = now_ms;
    input.speed_kmh = (model.speed_kmh_x10 != NAV_U16_UNKNOWN) ? (float)model.speed_kmh_x10 / 10.0f : 0.0f;
    input.speed_valid = model.speed_kmh_x10 != NAV_U16_UNKNOWN;
    input.maneuver_sequence = maneuver_identity(&model);
    countdown_accept(&input);

    out->icon_type = model.icon_type;
    out->speed_kmh_x10 = model.speed_kmh_x10;
    out->heading_deg = model.heading_deg;
    out->battery_percent = model.battery_percent;
    out->sequence = model.sequence;
    strncpy(out->street_name, model.street_name, sizeof(out->street_name) - 1);
    out->street_name[sizeof(out->street_name) - 1] = '\0';
    strncpy(out->eta, model.eta, sizeof(out->eta) - 1);
    out->eta[sizeof(out->eta) - 1] = '\0';

    if (model.icon_type == NAV_ICON_ARRIVED) {
        out->state = VIEW_ARRIVED;
        out->distance_meters = 0;
    } else if (!distance_valid && !countdown_has_baseline()) {
        // Never had a distance for this maneuver — nothing to count down from.
        out->state = VIEW_IDLE;
        out->distance_meters = 0;
    } else {
        // Includes the no-distance-but-we-have-an-anchor case: Maps drops the distance on the
        // final update before the turn, and blanking the screen at the moment of the maneuver is
        // the worst possible behaviour. Keep counting down from the last known distance.
        out->state = VIEW_ACTIVE;
    }

    view_state_tick(now_ms, out);
    drive_log_view(out);
}

void view_state_tick(uint32_t now_ms, terminal_view_state_t *out) {
    if (out->state != VIEW_ACTIVE && out->state != VIEW_STALE) {
        return;
    }
    countdown_output_t co = countdown_estimate(now_ms);
    out->distance_meters = co.distance_meters;
    out->stale = co.stale;
    if (co.stale && out->state == VIEW_ACTIVE) {
        out->state = VIEW_STALE;
    }
    // Called at 10Hz from countdown_task; drive_log_view() filters to actual changes.
    drive_log_view(out);
}
