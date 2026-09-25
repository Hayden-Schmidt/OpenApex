#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "../main/heading_fusion.h"

static heading_input_t base_input(uint32_t now_ms) {
    heading_input_t in = {0};
    in.timestamp_ms = now_ms;
    return in;
}

// Shortest signed angular delta from a to b, in (-180, 180] -- local copy, not exported by
// heading_fusion.h since it's an internal detail of the module under test.
static float angle_delta_deg(float a, float b) {
    float d = fmodf(b - a + 180.0f, 360.0f);
    if (d < 0.0f) d += 360.0f;
    return d - 180.0f;
}

static float wrap360(float deg) {
    float d = fmodf(deg, 360.0f);
    if (d < 0.0f) d += 360.0f;
    return d;
}

#define HF_TEST_PI 3.14159265358979323846

static float vec_to_deg(float x, float y) {
    return wrap360((float)(atan2(y, x) * 180.0 / HF_TEST_PI));
}

int main(void) {
    // 1. Circular low-pass crosses 0/360 the short way, not by sweeping through 180.
    heading_fusion_reset();
    heading_input_t in = base_input(0);
    in.gps_course_valid = true;
    in.gps_course_deg = 358.0f;
    in.speed_valid = true;
    in.speed_kmh = 30.0f;
    heading_fusion_accept(&in);
    heading_fusion_estimate(0);

    in.timestamp_ms = 200;
    in.gps_course_deg = 2.0f; // wraps past 0/360
    heading_fusion_accept(&in);
    heading_output_t out = heading_fusion_estimate(200);
    assert(out.valid);
    // Should land close to the 358/2 pair via the short way through 0, never near 180.
    assert(fabsf(angle_delta_deg(358.0f, out.heading_deg)) < 10.0f ||
           fabsf(angle_delta_deg(2.0f, out.heading_deg)) < 10.0f);

    // 2. Offset estimator converges toward a known synthetic offset (gps_course - yaw), given
    // clean, straight, fast, accurate-GPS inputs.
    heading_fusion_reset();
    const float true_offset = 40.0f;
    const float gps_course = 90.0f;
    for (int i = 0; i < 400; i++) {
        heading_input_t s = base_input((uint32_t)(i * 200));
        s.gps_course_valid = true;
        s.gps_course_deg = gps_course;
        s.gps_accuracy_valid = true;
        s.gps_accuracy_deg = 1.0f;
        s.speed_valid = true;
        s.speed_kmh = 40.0f;
        s.yaw_valid = true;
        s.yaw_deg = wrap360(gps_course - true_offset); // constant, straight riding
        s.yaw_rate_valid = true;
        s.yaw_rate_dps = 0.0f;
        heading_fusion_accept(&s);
    }
    heading_fusion_state_t converged;
    heading_fusion_get_state(&converged);
    assert(converged.have_offset);
    float learned_offset_deg = vec_to_deg(converged.offset_x, converged.offset_y);
    assert(fabsf(angle_delta_deg(true_offset, learned_offset_deg)) < 5.0f);
    float converged_r = hypotf(converged.offset_x, converged.offset_y);

    // Variance rises (r drops) when yaw is jittered (jacket-flap case) even though the true
    // average offset is unchanged.
    heading_fusion_reset();
    for (int i = 0; i < 400; i++) {
        heading_input_t s = base_input((uint32_t)(i * 200));
        s.gps_course_valid = true;
        s.gps_course_deg = gps_course;
        s.gps_accuracy_valid = true;
        s.gps_accuracy_deg = 1.0f;
        s.speed_valid = true;
        s.speed_kmh = 40.0f;
        s.yaw_valid = true;
        float jitter = (i % 2 == 0) ? 15.0f : -15.0f;
        s.yaw_deg = wrap360(gps_course - true_offset + jitter);
        s.yaw_rate_valid = true;
        s.yaw_rate_dps = 0.0f;
        heading_fusion_accept(&s);
    }
    heading_fusion_state_t jittered;
    heading_fusion_get_state(&jittered);
    assert(jittered.have_offset);
    float jittered_r = hypotf(jittered.offset_x, jittered.offset_y);
    assert(jittered_r < converged_r);

    // 3. Slew-rate limit caps the displayed heading's movement per estimate() call.
    heading_fusion_reset();
    heading_input_t slew_in = base_input(0);
    slew_in.gps_course_valid = true;
    slew_in.gps_course_deg = 0.0f;
    slew_in.speed_valid = true;
    slew_in.speed_kmh = 30.0f;
    heading_fusion_accept(&slew_in);
    heading_fusion_estimate(0); // establishes displayed heading at 0

    slew_in.timestamp_ms = 100;
    slew_in.gps_course_deg = 170.0f; // large jump
    heading_fusion_accept(&slew_in);
    heading_output_t slew_out = heading_fusion_estimate(100); // only 100ms elapsed
    assert(slew_out.valid);
    // MAX_SLEW_DPS is 90 deg/s -> at most 9 degrees of movement in 100ms.
    assert(fabsf(angle_delta_deg(0.0f, slew_out.heading_deg)) <= 9.5f);

    // 4. True stillness: zero speed + near-zero yaw_rate held over a long duration must not drift.
    heading_fusion_reset();
    heading_input_t still = base_input(0);
    still.gps_course_valid = true;
    still.gps_course_deg = 45.0f;
    still.speed_valid = true;
    still.speed_kmh = 20.0f;
    heading_fusion_accept(&still);
    heading_fusion_estimate(0);

    still.timestamp_ms = 1000;
    still.speed_kmh = 0.0f;
    still.gps_course_valid = false; // GPS course is undefined at a standstill
    still.yaw_valid = true;
    still.yaw_deg = 45.0f;
    still.yaw_rate_valid = true;
    still.yaw_rate_dps = 0.2f; // gyro bias noise, not real rotation
    for (int i = 0; i < 300; i++) { // 30 simulated seconds parked
        still.timestamp_ms += 100;
        heading_fusion_accept(&still);
    }
    heading_output_t still_out = heading_fusion_estimate(still.timestamp_ms);
    assert(still_out.valid);
    assert(still_out.frozen);
    assert(fabsf(angle_delta_deg(45.0f, still_out.heading_deg)) < 0.5f);

    // 5. Real low-speed rotation: near-zero speed but a sustained non-trivial yaw_rate (a slow
    // U-turn / parking maneuver) must actually rotate the output, not freeze.
    heading_fusion_reset();
    heading_input_t rot = base_input(0);
    rot.gps_course_valid = true;
    rot.gps_course_deg = 0.0f;
    rot.speed_valid = true;
    rot.speed_kmh = 20.0f;
    rot.yaw_valid = true;
    rot.yaw_deg = 0.0f;
    rot.yaw_rate_valid = true;
    rot.yaw_rate_dps = 0.0f;
    // Establish a converged (zero) mount offset first by riding straight at speed.
    for (int i = 0; i < 400; i++) {
        rot.timestamp_ms = (uint32_t)(i * 200);
        heading_fusion_accept(&rot);
    }
    heading_fusion_estimate(rot.timestamp_ms);

    // Now walk the bike around in a slow turn: near-zero speed, sustained real yaw rate.
    float yaw = 0.0f;
    for (int i = 0; i < 50; i++) {
        rot.timestamp_ms += 100;
        rot.speed_valid = true;
        rot.speed_kmh = 0.5f;
        rot.gps_course_valid = false;
        yaw = wrap360(yaw + 3.0f); // 30 deg/s sustained rotation
        rot.yaw_deg = yaw;
        rot.yaw_rate_dps = 30.0f;
        heading_fusion_accept(&rot);
    }
    heading_output_t rot_out = heading_fusion_estimate(rot.timestamp_ms);
    assert(rot_out.valid);
    assert(!rot_out.frozen);
    // Heading must have moved meaningfully from 0, tracking the sustained rotation.
    assert(fabsf(angle_delta_deg(0.0f, rot_out.heading_deg)) > 20.0f);

    // 6. State round-trip: get_state() -> restore_state() into a fresh instance reproduces the
    // same subsequent output as the original.
    heading_fusion_reset();
    heading_input_t warm = base_input(0);
    warm.gps_course_valid = true;
    warm.gps_course_deg = 120.0f;
    warm.speed_valid = true;
    warm.speed_kmh = 30.0f;
    warm.yaw_valid = true;
    warm.yaw_deg = 100.0f;
    warm.yaw_rate_valid = true;
    warm.yaw_rate_dps = 0.0f;
    for (int i = 0; i < 300; i++) {
        warm.timestamp_ms = (uint32_t)(i * 200);
        heading_fusion_accept(&warm);
    }
    heading_output_t before = heading_fusion_estimate(warm.timestamp_ms);
    heading_fusion_state_t saved;
    heading_fusion_get_state(&saved);

    heading_fusion_reset(); // simulate a fresh boot
    heading_fusion_restore_state(&saved);
    heading_output_t restored_first = heading_fusion_estimate(0);
    assert(restored_first.valid);
    // Immediately after restore, the display shows the cached heading, not a slew-in from zero.
    assert(fabsf(angle_delta_deg(before.heading_deg, restored_first.heading_deg)) < 0.5f);

    // Feeding the same next packet into both should converge to the same place.
    heading_input_t next = warm;
    next.timestamp_ms = warm.timestamp_ms + 200;
    next.gps_course_deg = 125.0f;
    next.yaw_deg = 105.0f;
    heading_fusion_accept(&next);
    heading_output_t after_restored = heading_fusion_estimate(next.timestamp_ms);
    assert(after_restored.valid);
    assert(fabsf(angle_delta_deg(before.heading_deg, after_restored.heading_deg)) < 15.0f);

    puts("heading_fusion tests passed");
    return 0;
}
