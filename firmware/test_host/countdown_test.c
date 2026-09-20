#include <assert.h>
#include <stdio.h>

#include "../main/countdown.h"

int main(void) {
    countdown_reset();
    countdown_input_t input = {
        .distance_meters = 100,
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
    puts("countdown tests passed");
    return 0;
}
