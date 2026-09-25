#pragma once

#include <cmath>
#include <cstdint>

#include "lvgl.h"

// Easing curves for page transitions. "design reference/Screen Transitions.md": every per-element
// motion is an S-curve or an exponential, never linear. Plain float forms for tweens driven by hand
// (NavRenderer, the ring zoom), and anim_path<> to hand the same curve to an lv_anim.

inline float ease_in_out_cubic(float t) {
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f;
}

// Inverse-exponential: fast start, settles gently. Used for things arriving from off the page.
inline float ease_out_expo(float t) {
    return t >= 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t);
}

// Exponential: slow start, accelerates into the end. Used where motion should close in hard.
inline float ease_in_expo(float t) {
    return t <= 0.0f ? 0.0f : std::pow(2.0f, 10.0f * (t - 1.0f));
}

template <float (*Ease)(float)>
int32_t anim_path(const lv_anim_t *a) {
    float t = a->duration > 0 ? static_cast<float>(a->act_time) / static_cast<float>(a->duration)
                              : 1.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float delta = static_cast<float>(a->end_value - a->start_value);
    return a->start_value + static_cast<int32_t>(std::lround(delta * Ease(t)));
}
