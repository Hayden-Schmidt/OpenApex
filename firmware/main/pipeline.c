#include "pipeline.h"

#include <string.h>

#include "normalize.h"
#include "packet.h"

void view_state_apply_packet(const raw_notif_t *raw, uint32_t now_ms, terminal_view_state_t *out) {
    nav_model_t model;
    normalize_packet(raw, &model);

    // Android's Notification.ProgressStyle (current Google Maps) carries no plain distance text
    // (shortCriticalText is empty) -- only progress/progressMax, which normalize_packet turns into
    // remaining_meters. Fall back to it whenever the per-maneuver distance field is unknown.
    int32_t distance_meters = model.distance_meters >= 0 ? model.distance_meters : model.remaining_meters;

    countdown_input_t input;
    memset(&input, 0, sizeof(input));
    input.distance_meters = (distance_meters >= 0) ? (uint32_t)distance_meters : 0U;
    input.timestamp_ms = now_ms;
    input.speed_kmh = (model.speed_kmh_x10 != NAV_U16_UNKNOWN) ? (float)model.speed_kmh_x10 / 10.0f : 0.0f;
    input.speed_valid = model.speed_kmh_x10 != NAV_U16_UNKNOWN;
    input.maneuver_sequence = model.sequence;
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
    } else if (distance_meters < 0) {
        // No distance to count down — cannot show an active maneuver without it.
        out->state = VIEW_IDLE;
        out->distance_meters = 0;
    } else {
        out->state = VIEW_ACTIVE;
    }

    view_state_tick(now_ms, out);
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
}
