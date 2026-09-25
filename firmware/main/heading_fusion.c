#include "heading_fusion.h"

#include <math.h>

// Speed tiers (docs/Heading_Sensor_Fusion_Plan.md §3). Above SPEED_TRUST_KMH, GPS course noise is
// low enough to treat as truth; below SPEED_FREEZE_KMH, GPS course is undefined/noise and the
// display holds the last good heading unless yaw_rate says the bike is actually rotating.
#define SPEED_TRUST_KMH 15.0f
#define SPEED_FREEZE_KMH 3.0f

// Below this yaw rate, a stationary bike's gyro reading is bias noise, not rotation -- holding
// still here is what keeps a parked bike from drifting as if it were slowly spinning.
#define YAW_RATE_STATIONARY_NOISE_DPS 1.5f
// Above this yaw rate, the bike is mid-corner: gate the offset estimator off so a turn never gets
// mistaken for a mount-offset shift.
#define YAW_RATE_STRAIGHT_DPS 5.0f
// Offset learning also requires the GPS course itself to be trustworthy.
#define GPS_ACCURACY_GATE_DEG 5.0f
// Confidence scaling ceiling: bearing accuracy at or above this is "poor" (confidence -> 0).
#define GPS_ACCURACY_POOR_DEG 10.0f

// Mount-offset time constant, 30-60s per the design doc; the midpoint self-learns every ride with
// no calibration ritual and re-converges when the phone moves to a different pocket.
#define OFFSET_TIME_CONSTANT_S 45.0f

// Circular low-pass time constants for the filtered heading, by regime.
#define TAU_FAST_S 1.5f          // >= SPEED_TRUST_KMH: GPS is truth, track it tightly
#define TAU_BLEND_S 3.0f         // 3-15 km/h blend zone, and GPS-dropout-at-speed fallback to yaw
#define TAU_LOW_SPEED_ROTATE_S 1.0f  // near-stationary but genuinely rotating: yaw is the only source

// Display-side slew limit (§4 non-negotiables): heading arrives with each accept(), estimate() is
// called at 10Hz, and without a cap the ~1Hz updates look like steps.
#define MAX_SLEW_DPS 90.0f

// Clamp elapsed time used in any EMA/slew calc so a long GNSS/link gap (reconnect, tunnel) cannot
// slam the filter with one giant step disguised as a normal update.
#define MAX_DT_S 5.0f

typedef struct {
    float x, y;
} vec2_t;

// Not M_PI: -std=c11 (no POSIX/GNU extensions) leaves it undefined on some toolchains (MinGW).
#define HF_PI 3.14159265358979323846f

static float deg2rad(float d) { return d * HF_PI / 180.0f; }
static float rad2deg(float r) { return r * 180.0f / HF_PI; }

static float wrap360(float deg) {
    float d = fmodf(deg, 360.0f);
    if (d < 0.0f) d += 360.0f;
    return d;
}

static vec2_t vec_from_deg(float deg) {
    float r = deg2rad(deg);
    return (vec2_t){cosf(r), sinf(r)};
}

static float deg_from_vec(vec2_t v) {
    return wrap360(rad2deg(atan2f(v.y, v.x)));
}

static vec2_t vec_add_scaled(vec2_t a, float wa, vec2_t b, float wb) {
    return (vec2_t){a.x * wa + b.x * wb, a.y * wa + b.y * wb};
}

// Shortest signed angular delta from a to b, in (-180, 180].
static float angle_delta_deg(float a, float b) {
    float d = fmodf(b - a + 180.0f, 360.0f);
    if (d < 0.0f) d += 360.0f;
    return d - 180.0f;
}

typedef struct {
    float filtered_x, filtered_y;
    bool have_filtered;

    float last_good_deg;
    bool have_last_good;

    float displayed_x, displayed_y;
    bool have_displayed;
    uint32_t displayed_timestamp_ms;
    bool have_displayed_timestamp;

    float offset_x, offset_y;
    bool have_offset;

    uint32_t last_accept_timestamp_ms;
    bool have_last_accept;

    // Held between accept() calls so estimate() (called far more often, at 10Hz) reports the most
    // recently computed regime rather than re-deriving it without a fresh packet to derive it from.
    float last_confidence;
    bool last_frozen;
    bool ever_valid;
} state_t;

static state_t s;

void heading_fusion_reset(void) {
    s = (state_t){0};
}

// Circular variance of the offset estimator: 0 = tight agreement every sample, 1 = no agreement at
// all. Derived from the EMA vector's own magnitude rather than stored -- it is fully determined by
// offset_x/offset_y, so storing it separately would just be a second copy that can drift out of
// sync with them.
static float offset_variance(void) {
    if (!s.have_offset) return 1.0f;
    float r = hypotf(s.offset_x, s.offset_y);
    if (r > 1.0f) r = 1.0f;
    return 1.0f - r;
}

static float offset_angle_deg(void) {
    return deg_from_vec((vec2_t){s.offset_x, s.offset_y});
}

// Blends up to two candidate unit-vector targets, weighting `a` by `weight_a` when both are
// present. Falls back to whichever one is actually available; returns false if neither is.
static bool blend_target(bool a_ok, vec2_t a, bool b_ok, vec2_t b, float weight_a, vec2_t *out) {
    if (a_ok && b_ok) {
        vec2_t sum = vec_add_scaled(a, weight_a, b, 1.0f - weight_a);
        float len = hypotf(sum.x, sum.y);
        if (len < 1e-3f) {
            // Degenerate case: the two candidates are ~180 deg apart and cancel out. Prefer GPS
            // (candidate a) as the more trustworthy absolute reference in that tie.
            *out = a;
            return true;
        }
        *out = (vec2_t){sum.x / len, sum.y / len};
        return true;
    }
    if (a_ok) { *out = a; return true; }
    if (b_ok) { *out = b; return true; }
    return false;
}

void heading_fusion_accept(const heading_input_t *input) {
    if (input == NULL) return;

    float dt_s = 0.0f;
    if (s.have_last_accept) {
        uint32_t elapsed_ms = input->timestamp_ms - s.last_accept_timestamp_ms;
        dt_s = (float)elapsed_ms / 1000.0f;
        if (dt_s < 0.0f) dt_s = 0.0f;
        if (dt_s > MAX_DT_S) dt_s = MAX_DT_S;
    }
    s.last_accept_timestamp_ms = input->timestamp_ms;
    s.have_last_accept = true;

    const bool gps_ok = input->gps_course_valid;
    const bool yaw_raw_ok = input->yaw_valid;
    const bool yaw_bike_ok = yaw_raw_ok && s.have_offset;
    const vec2_t gps_vec = gps_ok ? vec_from_deg(input->gps_course_deg) : (vec2_t){0};
    const vec2_t yaw_vec = yaw_bike_ok
        ? vec_from_deg(wrap360(input->yaw_deg + offset_angle_deg()))
        : (vec2_t){0};

    // 1. Learn the mount offset, only while the evidence is trustworthy (doc §1): riding straight
    //    at speed, with a GPS course accurate enough to trust as ground truth.
    const bool riding_straight = !input->yaw_rate_valid || fabsf(input->yaw_rate_dps) < YAW_RATE_STRAIGHT_DPS;
    const bool gps_accurate_enough = !input->gps_accuracy_valid || input->gps_accuracy_deg < GPS_ACCURACY_GATE_DEG;
    const bool can_learn_offset = gps_ok && yaw_raw_ok && input->yaw_rate_valid &&
        input->speed_valid && input->speed_kmh > SPEED_TRUST_KMH &&
        gps_accurate_enough && riding_straight;
    if (can_learn_offset && dt_s > 0.0f) {
        float sample_deg = wrap360(input->gps_course_deg - input->yaw_deg);
        vec2_t sample = vec_from_deg(sample_deg);
        float alpha = 1.0f - expf(-dt_s / OFFSET_TIME_CONSTANT_S);
        if (!s.have_offset) {
            s.offset_x = sample.x;
            s.offset_y = sample.y;
        } else {
            s.offset_x = s.offset_x * (1.0f - alpha) + sample.x * alpha;
            s.offset_y = s.offset_y * (1.0f - alpha) + sample.y * alpha;
        }
        s.have_offset = true;
        // Recompute the bike-frame yaw target with the just-updated offset for this same sample.
    }
    const vec2_t yaw_vec_fresh = (yaw_raw_ok && s.have_offset)
        ? vec_from_deg(wrap360(input->yaw_deg + offset_angle_deg()))
        : yaw_vec;
    const bool yaw_bike_ok_fresh = yaw_raw_ok && s.have_offset;

    // 2. Pick this update's target heading and low-pass time constant by speed regime (doc §3).
    vec2_t target = {0};
    float tau_s = TAU_BLEND_S;
    bool have_target = false;
    bool frozen = false;

    if (input->speed_valid && input->speed_kmh >= SPEED_TRUST_KMH) {
        have_target = blend_target(gps_ok, gps_vec, yaw_bike_ok_fresh, yaw_vec_fresh, 1.0f, &target);
        tau_s = gps_ok ? TAU_FAST_S : TAU_BLEND_S;
    } else if (input->speed_valid && input->speed_kmh > SPEED_FREEZE_KMH) {
        float t = (input->speed_kmh - SPEED_FREEZE_KMH) / (SPEED_TRUST_KMH - SPEED_FREEZE_KMH);
        have_target = blend_target(gps_ok, gps_vec, yaw_bike_ok_fresh, yaw_vec_fresh, t, &target);
        tau_s = TAU_BLEND_S;
    } else if (input->speed_valid) {
        // <= SPEED_FREEZE_KMH: freeze unless yaw_rate shows genuine rotation (walking the bike
        // around, a slow U-turn, a low-speed roundabout exit) -- see the design doc's low-speed
        // refinement. GPS course is not used here at all; it is noise in this regime.
        const bool genuinely_rotating = input->yaw_rate_valid &&
            fabsf(input->yaw_rate_dps) > YAW_RATE_STATIONARY_NOISE_DPS && yaw_bike_ok_fresh;
        if (genuinely_rotating) {
            target = yaw_vec_fresh;
            tau_s = TAU_LOW_SPEED_ROTATE_S;
            have_target = true;
        } else {
            have_target = false;
            frozen = true;
        }
    } else {
        // No speed reading at all: use whatever absolute source is available, conservatively.
        have_target = blend_target(gps_ok, gps_vec, yaw_bike_ok_fresh, yaw_vec_fresh, 0.5f, &target);
        tau_s = TAU_BLEND_S;
    }

    if (have_target) {
        if (!s.have_filtered) {
            s.filtered_x = target.x;
            s.filtered_y = target.y;
        } else {
            float alpha = (dt_s > 0.0f) ? (1.0f - expf(-dt_s / tau_s)) : 1.0f;
            vec2_t blended = vec_add_scaled((vec2_t){s.filtered_x, s.filtered_y}, 1.0f - alpha, target, alpha);
            float len = hypotf(blended.x, blended.y);
            if (len < 1e-3f) {
                s.filtered_x = target.x;
                s.filtered_y = target.y;
            } else {
                s.filtered_x = blended.x / len;
                s.filtered_y = blended.y / len;
            }
        }
        s.have_filtered = true;
        s.last_good_deg = deg_from_vec((vec2_t){s.filtered_x, s.filtered_y});
        s.have_last_good = true;
        s.ever_valid = true;
    } else if (s.have_last_good) {
        // Frozen: filtered stays pinned to the held heading -- never fabricate drift from a target
        // we deliberately chose not to trust this tick.
        vec2_t held = vec_from_deg(s.last_good_deg);
        s.filtered_x = held.x;
        s.filtered_y = held.y;
        s.have_filtered = true;
    }

    // 3. Confidence: how much to trust this update, independent of whether it moved anything.
    float gps_component = 0.0f;
    if (gps_ok) {
        gps_component = input->gps_accuracy_valid
            ? fmaxf(0.0f, fminf(1.0f, 1.0f - input->gps_accuracy_deg / GPS_ACCURACY_POOR_DEG))
            : 0.8f;
    }
    float yaw_component = yaw_bike_ok_fresh ? fmaxf(0.0f, 1.0f - offset_variance()) : 0.0f;

    float confidence;
    if (input->speed_valid && input->speed_kmh >= SPEED_TRUST_KMH) {
        confidence = gps_ok ? fmaxf(gps_component, 0.7f) : yaw_component;
    } else if (input->speed_valid && input->speed_kmh > SPEED_FREEZE_KMH) {
        float t = (input->speed_kmh - SPEED_FREEZE_KMH) / (SPEED_TRUST_KMH - SPEED_FREEZE_KMH);
        confidence = t * gps_component + (1.0f - t) * yaw_component;
    } else if (frozen) {
        confidence = s.have_last_good ? 0.6f : 0.0f;
    } else if (have_target) {
        // Genuinely rotating near-stationary: fully dependent on how well the mount offset is known.
        confidence = yaw_component;
    } else {
        confidence = gps_ok ? gps_component : yaw_component;
    }

    s.last_confidence = confidence;
    s.last_frozen = frozen;
}

heading_output_t heading_fusion_estimate(uint32_t now_ms) {
    heading_output_t out = {0};
    if (!s.have_filtered) {
        return out; // valid stays false: never fabricate a heading before the first accept()
    }

    float dt_s = 0.0f;
    if (s.have_displayed_timestamp) {
        uint32_t elapsed_ms = now_ms - s.displayed_timestamp_ms;
        dt_s = (float)elapsed_ms / 1000.0f;
        if (dt_s < 0.0f) dt_s = 0.0f;
        if (dt_s > MAX_DT_S) dt_s = MAX_DT_S;
    }
    s.displayed_timestamp_ms = now_ms;
    s.have_displayed_timestamp = true;

    float target_deg = deg_from_vec((vec2_t){s.filtered_x, s.filtered_y});
    if (!s.have_displayed) {
        // First tick (including right after a restore from cache): show the cached/filtered
        // heading immediately rather than slewing in from nothing.
        s.displayed_x = s.filtered_x;
        s.displayed_y = s.filtered_y;
        s.have_displayed = true;
    } else {
        float current_deg = deg_from_vec((vec2_t){s.displayed_x, s.displayed_y});
        float delta = angle_delta_deg(current_deg, target_deg);
        float max_step = MAX_SLEW_DPS * dt_s;
        float step = fmaxf(-max_step, fminf(max_step, delta));
        vec2_t moved = vec_from_deg(wrap360(current_deg + step));
        s.displayed_x = moved.x;
        s.displayed_y = moved.y;
    }

    out.heading_deg = deg_from_vec((vec2_t){s.displayed_x, s.displayed_y});
    out.valid = s.ever_valid;
    out.confidence = s.last_confidence;
    out.frozen = s.last_frozen;
    return out;
}

void heading_fusion_get_state(heading_fusion_state_t *out) {
    if (out == NULL) return;
    out->filtered_x = s.filtered_x;
    out->filtered_y = s.filtered_y;
    out->have_filtered = s.have_filtered;
    out->last_good_deg = s.last_good_deg;
    out->have_last_good = s.have_last_good;
    out->offset_x = s.offset_x;
    out->offset_y = s.offset_y;
    out->have_offset = s.have_offset;
}

void heading_fusion_restore_state(const heading_fusion_state_t *saved) {
    if (saved == NULL) return;
    s.filtered_x = saved->filtered_x;
    s.filtered_y = saved->filtered_y;
    s.have_filtered = saved->have_filtered;
    s.last_good_deg = saved->last_good_deg;
    s.have_last_good = saved->have_last_good;
    s.offset_x = saved->offset_x;
    s.offset_y = saved->offset_y;
    s.have_offset = saved->have_offset;
    // The restored offset/filtered heading is treated as an already-converged prior, not a live
    // reading: ever_valid stays true (there's a real cached heading to show), but the display and
    // accept-side timing bookkeeping restart clean so the first post-boot tick shows the cached
    // heading immediately (see heading_fusion_estimate) and the first accept() doesn't compute its
    // dt against a stale pre-boot timestamp.
    s.ever_valid = saved->have_filtered;
    s.have_displayed = false;
    s.have_displayed_timestamp = false;
    s.have_last_accept = false;
    s.last_confidence = saved->have_filtered ? 0.6f : 0.0f;
    s.last_frozen = true;
}
