#include <assert.h>
#include <stdio.h>

#include "../main/countdown.h"

int main(void) {
    countdown_reset();
    countdown_input_t input = {
        .distance_meters = 100,
        .distance_valid = true,
        .timestamp_ms = 0,
        .speed_kmh = 36.0f,
        .speed_valid = true,
        .maneuver_sequence = 1,
    };
    countdown_accept(&input);
    countdown_output_t output = countdown_estimate(5000);
    assert(output.distance_meters < 100);
    assert(!output.stale);
    output = countdown_estimate(11000);
    assert(output.stale);

    // Duplicate packets (same distance, e.g. the same "450m" notification relayed repeatedly)
    // must not reset the distance interpolation's elapsed-time anchor. Without debouncing this
    // would keep the displayed distance frozen near 100m instead of counting down.
    countdown_reset();
    countdown_input_t dup = {
        .distance_meters = 100,
        .distance_valid = true,
        .timestamp_ms = 0,
        .speed_kmh = 36.0f, // 10 m/s
        .speed_valid = true,
        .maneuver_sequence = 1,
    };
    countdown_accept(&dup);
    dup.timestamp_ms = 2000;
    countdown_accept(&dup); // duplicate distance -- must not move the anchor to t=2000
    dup.timestamp_ms = 4000;
    countdown_accept(&dup); // duplicate distance -- must not move the anchor to t=4000
    output = countdown_estimate(5000);
    // Anchor stayed at t=0: travelled = 10 m/s * 5s = 50m -> ~50m remaining.
    assert(output.distance_meters >= 45 && output.distance_meters <= 55);
    assert(!output.stale); // packets kept arriving, so liveness is still fresh

    // Google Maps drops the distance text on the final update before a turn (you are AT the turn).
    // That packet must not re-anchor the countdown: the display has to keep ticking down from the
    // last real distance. Regression for the on-road fault where it jumped from ~10m to ~5000m,
    // because the whole-trip remaining distance was substituted for the maneuver distance.
    countdown_reset();
    countdown_input_t approach = {
        .distance_meters = 10,
        .distance_valid = true,
        .timestamp_ms = 0,
        .speed_kmh = 18.0f, // 5 m/s
        .speed_valid = true,
        .maneuver_sequence = 7,
    };
    countdown_accept(&approach);

    countdown_input_t at_turn = approach;
    at_turn.distance_valid = false;
    at_turn.distance_meters = 0; // caller's stand-in value -- must be ignored entirely
    at_turn.timestamp_ms = 1000;
    countdown_accept(&at_turn);

    output = countdown_estimate(1000);
    // Anchor held at 10m from t=0: travelled = 5 m/s * 1s = 5m -> ~5m remaining. Crucially small.
    assert(output.distance_meters <= 10);
    assert(!output.stale);

    // Speed smoothing must actually smooth. The maneuver id identifies a MANEUVER, not a packet:
    // when it holds steady across packets, a sudden reported-speed change has to be filtered, not
    // adopted outright. Passing the per-packet sequence here (as the pipeline used to) made every
    // packet look like a new maneuver and silently disabled the 0.7/0.3 filter entirely.
    countdown_reset();
    countdown_input_t cruise = {
        .distance_meters = 500,
        .distance_valid = true,
        .timestamp_ms = 0,
        .speed_kmh = 50.0f,
        .speed_valid = true,
        .maneuver_sequence = 42,
    };
    countdown_accept(&cruise);
    // Same maneuver, speed suddenly reads zero (a GNSS dropout, not an actual stop).
    cruise.speed_kmh = 0.0f;
    for (int i = 1; i <= 4; i++) {
        cruise.timestamp_ms = (uint32_t)(i * 100);
        countdown_accept(&cruise);
    }
    output = countdown_estimate(500);
    // Smoothed speed is still non-zero, so the distance kept ticking down from 500m.
    assert(output.distance_meters < 500);

    // Same sequence of packets, but each one claiming a different maneuver: no smoothing across a
    // genuine maneuver change, so the zero is adopted immediately and nothing is travelled.
    countdown_reset();
    cruise.speed_kmh = 50.0f;
    cruise.timestamp_ms = 0;
    cruise.maneuver_sequence = 1;
    countdown_accept(&cruise);
    cruise.speed_kmh = 0.0f;
    for (int i = 1; i <= 4; i++) {
        cruise.timestamp_ms = (uint32_t)(i * 100);
        cruise.maneuver_sequence = (uint32_t)(i + 1);
        countdown_accept(&cruise);
    }
    output = countdown_estimate(500);
    assert(output.distance_meters == 500);

    puts("countdown tests passed");
    return 0;
}
