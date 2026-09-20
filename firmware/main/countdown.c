#include "countdown.h"

#include <math.h>

#define MAX_HOLD_MS 10000U
#define MAX_SPEED_KMH 240.0f

static countdown_input_t current;
static float filtered_speed_kmh;
static bool has_baseline;

void countdown_reset(void) {
    current = (countdown_input_t){0};
    filtered_speed_kmh = 0.0f;
    has_baseline = false;
}

void countdown_accept(const countdown_input_t *input) {
    if (input == NULL) {
        return;
    }
    bool new_maneuver = has_baseline && input->maneuver_sequence != current.maneuver_sequence;
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

countdown_output_t countdown_estimate(uint32_t now_ms) {
    if (!has_baseline) {
        return (countdown_output_t){0, true};
    }

    uint32_t elapsed_ms = now_ms - current.timestamp_ms;
    bool stale = elapsed_ms > MAX_HOLD_MS || !current.speed_valid;
    uint32_t bounded_elapsed = stale ? MAX_HOLD_MS : elapsed_ms;
    float travelled = (filtered_speed_kmh / 3.6f) * ((float)bounded_elapsed / 1000.0f);
    uint32_t estimate = travelled >= (float)current.distance_meters
        ? 0U
        : current.distance_meters - (uint32_t)travelled;
    return (countdown_output_t){estimate, stale};
}
