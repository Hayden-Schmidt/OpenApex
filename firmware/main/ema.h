#pragma once

#include <math.h>

// Exponential smoothing step shared by heading_fusion (compass) and mesh_core (friend offsets).
// Header-only, no ESP-IDF dependency, so host tests and the simulator use it as-is.

// Weight of a new sample after `dt_s` seconds with time constant `tau_s`. A non-positive `dt_s`
// gives 1 (take the sample whole), which is also what a first sample wants.
static inline float ema_alpha(float dt_s, float tau_s) {
    return dt_s > 0.0f ? 1.0f - expf(-dt_s / tau_s) : 1.0f;
}

static inline float ema_blend(float prev, float sample, float alpha) {
    return prev * (1.0f - alpha) + sample * alpha;
}
