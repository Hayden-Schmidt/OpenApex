#include "odometer.h"

static double s_meters;
static bool s_have_prev;
static float s_prev_speed_kmh;
static uint32_t s_prev_ms;

void odometer_reset(void) {
    s_meters = 0.0;
    s_have_prev = false;
}

void odometer_accept(bool fix_valid, bool speed_valid, float speed_kmh, uint32_t now_ms) {
    if (!fix_valid || !speed_valid) {
        s_have_prev = false;
        return;
    }
    if (s_have_prev) {
        uint32_t dt_ms = now_ms - s_prev_ms;
        if (dt_ms <= ODOMETER_MAX_GAP_MS) {
            // Trapezoid between the two samples, with each end clamped to zero below the
            // standing-still threshold.
            float a = s_prev_speed_kmh >= ODOMETER_MIN_SPEED_KMH ? s_prev_speed_kmh : 0.0f;
            float b = speed_kmh >= ODOMETER_MIN_SPEED_KMH ? speed_kmh : 0.0f;
            s_meters += ((double)(a + b) / 2.0) / 3.6 * ((double)dt_ms / 1000.0);
        }
    }
    s_prev_speed_kmh = speed_kmh;
    s_prev_ms = now_ms;
    s_have_prev = true;
}

uint32_t odometer_meters(void) {
    return (uint32_t)s_meters;
}

void odometer_restore(uint32_t meters) {
    s_meters = (double)meters;
}
