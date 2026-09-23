#include "countdown.h"

#include <math.h>

#define MAX_HOLD_MS 10000U
#define MAX_SPEED_KMH 240.0f

static countdown_input_t current;         // latest accepted packet; drives speed + liveness/staleness
static uint32_t anchor_distance_meters;   // distance interpolation baseline
static uint32_t anchor_timestamp_ms;      // ...and the timestamp it was set at
static float filtered_speed_kmh;
static bool has_baseline;

void countdown_reset(void) {
    current = (countdown_input_t){0};
    anchor_distance_meters = 0;
    anchor_timestamp_ms = 0;
    filtered_speed_kmh = 0.0f;
    has_baseline = false;
}

void countdown_accept(const countdown_input_t *input) {
    if (input == NULL) {
        return;
    }
    bool new_maneuver = has_baseline && input->maneuver_sequence != current.maneuver_sequence;

    // The phone relay resends the same notification content multiple times per second (BLE relay
    // tick / duplicate onNotificationPosted firing), so most packets carry an unchanged distance.
    // Re-anchoring distance_meters/timestamp_ms on every one of those would repeatedly reset the
    // elapsed-time baseline to ~0, freezing the interpolated distance instead of ticking it down
    // between genuine updates. Only move the distance anchor when the reported distance actually
    // changes; `current` (and the staleness check below) still updates on every packet so speed
    // smoothing and liveness detection don't regress.
    //
    // A packet with no distance at all (distance_valid == false) never moves the anchor: that is
    // the final pre-turn update, where holding the last real distance and letting speed tick it
    // down is exactly right. Re-anchoring there would either freeze the display or, worse, latch
    // whatever stand-in value the caller passed.
    if (input->distance_valid &&
        (!has_baseline || input->distance_meters != anchor_distance_meters)) {
        anchor_distance_meters = input->distance_meters;
        anchor_timestamp_ms = input->timestamp_ms;
    }

    current = *input;
    if (current.speed_valid && isfinite(current.speed_kmh)) {
        float bounded = fminf(fmaxf(current.speed_kmh, 0.0f), MAX_SPEED_KMH);
        if (new_maneuver || !has_baseline) {
            // Fresh baseline: no speed smoothing across maneuver/source changes.
            filtered_speed_kmh = bounded;
        } else {
            filtered_speed_kmh = filtered_speed_kmh * 0.7f + bounded * 0.3f;
        }
    }
    has_baseline = true;
}

bool countdown_has_baseline(void) {
    return has_baseline;
}

countdown_output_t countdown_estimate(uint32_t now_ms) {
    if (!has_baseline) {
        return (countdown_output_t){0, true};
    }

    // Staleness tracks whether packets are still arriving at all -- independent of the distance
    // anchor, which intentionally holds steady across duplicate/unchanged-distance packets (see
    // countdown_accept). Without this split, sitting still with an unchanging distance (e.g. a red
    // light) would falsely read as a lost connection after MAX_HOLD_MS even though packets keep
    // arriving.
    //
    // Speed validity is deliberately NOT part of this: a missing GNSS speed only means the
    // distance can't be interpolated between packets (travelled stays 0 below), not that the link
    // is dead. Folding it in here made every drive where the phone never promoted to the location
    // FGS type (see RelayService.startGnss) report VIEW_STALE, which blanked the distance label
    // entirely -- the maneuver and distance we *do* have are still fresh and worth showing.
    uint32_t elapsed_since_packet_ms = now_ms - current.timestamp_ms;
    bool stale = elapsed_since_packet_ms > MAX_HOLD_MS;

    uint32_t elapsed_ms = now_ms - anchor_timestamp_ms;
    uint32_t bounded_elapsed = stale ? MAX_HOLD_MS : elapsed_ms;
    float travelled = (filtered_speed_kmh / 3.6f) * ((float)bounded_elapsed / 1000.0f);
    uint32_t estimate = travelled >= (float)anchor_distance_meters
        ? 0U
        : anchor_distance_meters - (uint32_t)travelled;
    return (countdown_output_t){estimate, stale};
}
