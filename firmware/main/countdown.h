#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t distance_meters;
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
