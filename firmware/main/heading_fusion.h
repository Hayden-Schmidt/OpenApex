#pragma once

#include <stdbool.h>
#include <stdint.h>

// Terminal-side heading fusion -- see docs/Heading_Sensor_Fusion_Plan.md for the design. Pure C,
// no platform dependencies, host-testable exactly like countdown.c. Blends GPS course-over-ground
// with phone/IMU yaw by learning the fixed mount offset between them (the two are different
// quantities in different frames -- see the plan's "naive blend" section for why averaging degrees
// directly does not work), rather than a speed-weighted average of the two angles.
//
// accept()/estimate() split mirrors countdown.c: accept() ingests a decoded packet's raw fields as
// they arrive; estimate() runs on every display tick (10Hz) and slew-rate-limits the interpolation
// between accept() calls, doing the one atan2 per call that turns the internal unit vector into
// degrees (see the plan's compute-cost section -- no trig anywhere else in the hot path).

typedef struct {
    float gps_course_deg;    bool gps_course_valid;    // Location.bearing; invalid at a standstill
    float gps_accuracy_deg;  bool gps_accuracy_valid;  // bearingAccuracyDegrees; smaller = better
    float speed_kmh;         bool speed_valid;
    float yaw_deg;            bool yaw_valid;           // phone TYPE_ROTATION_VECTOR yaw today,
                                                          // onboard IMU later -- same struct either way
    float yaw_rate_dps;       bool yaw_rate_valid;       // gyro Z, the axis heading actually turns on
    uint32_t timestamp_ms;
} heading_input_t;

typedef struct {
    float heading_deg;   // bike frame; meaningless when valid is false
    bool  valid;          // false only before the first ever accept() -- never fabricate a heading
    float confidence;    // 0..1, discounted by mount-offset variance and GPS/yaw availability
    bool  frozen;         // held from the last good estimate rather than live (stopped, no rotation)
} heading_output_t;

// Cross-boot persistence (docs/Heading_Sensor_Fusion_Plan.md, "cross-boot persistence"): the
// platform layer (heading_store.c) reads this once at boot and writes it periodically, so the
// terminal doesn't start every ride with an unknown heading and a mount offset that has to
// reconverge from scratch. Plain data, no pointers, so it round-trips through NVS as a fixed-size
// blob. Every field is meaningful on its own -- there is no cached/derived state omitted; in
// particular the offset estimator's circular variance is derived from offset_x/offset_y's
// magnitude on demand rather than stored, since it is fully determined by them.
typedef struct {
    float filtered_x, filtered_y;   // filtered heading, unit vector
    bool  have_filtered;
    float last_good_deg;             // held heading while frozen
    bool  have_last_good;
    float offset_x, offset_y;        // EMA-accumulated unit vector for (gps_course - yaw)
    bool  have_offset;
} heading_fusion_state_t;

void heading_fusion_reset(void);
void heading_fusion_accept(const heading_input_t *input);
heading_output_t heading_fusion_estimate(uint32_t now_ms);

void heading_fusion_get_state(heading_fusion_state_t *out);
void heading_fusion_restore_state(const heading_fusion_state_t *saved);
