#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t distance_meters;
    // False when the packet carried no distance for this maneuver. Google Maps drops the distance
    // text on the final update before a turn (you are AT the turn), so the last thing shown must
    // keep counting down from the previous anchor rather than re-anchoring on a fabricated value.
    // distance_meters is ignored entirely when this is false.
    bool distance_valid;
    uint32_t timestamp_ms;
    float speed_kmh;
    bool speed_valid;
    uint32_t maneuver_sequence;
} countdown_input_t;

typedef struct {
    uint32_t distance_meters;
    bool stale;
} countdown_output_t;

void countdown_reset(void);
void countdown_accept(const countdown_input_t *input);
countdown_output_t countdown_estimate(uint32_t now_ms);

/** True once a real distance has been anchored, i.e. there is something to count down from. */
bool countdown_has_baseline(void);
