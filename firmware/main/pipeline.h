#pragma once

#include <stdint.h>

#include "countdown.h"
#include "packet.h"
#include "view_state.h"

// Pure state transition: applies a decoded raw packet to the countdown engine and produces the
// latest view state. Host-testable — no FreeRTOS, BLE, or LVGL dependencies, and time is injected
// explicitly (now_ms is a local monotonic millisecond counter supplied by the caller). The
// countdown_task calls this on each decoded packet and view_state_tick on each tick.

// Applies a newly decoded packet, re-baselining the countdown engine.
void view_state_apply_packet(const raw_notif_t *raw, uint32_t now_ms, terminal_view_state_t *out);

/** Clears maneuver tracking and the countdown. Use instead of countdown_reset() alone. */
void pipeline_reset(void);

// What the ETA label shows. Maps only sends the arrival clock ("Arrive 22:52"); the time left is
// derived from it and the phone's synced wall clock, since trip progress is a fraction with no
// duration behind it. Falls back to the arrival format while the phone clock is unknown.
typedef enum {
    PIPELINE_ETA_ARRIVAL = 0,    // "ETA 03:06"
    PIPELINE_ETA_TIME_LEFT = 1,  // "15 min"
} pipeline_eta_format_t;

#ifndef PIPELINE_ETA_FORMAT_DEFAULT
#define PIPELINE_ETA_FORMAT_DEFAULT PIPELINE_ETA_TIME_LEFT
#endif

/** Selects the ETA label format. Survives pipeline_reset(). */
void pipeline_set_eta_format(pipeline_eta_format_t format);

// Advances countdown interpolation to now_ms and refreshes distance/stale in out.
void view_state_tick(uint32_t now_ms, terminal_view_state_t *out);

// Monotonic millisecond clock. On the ESP32 this wraps esp_timer_get_time(); host tests provide
// their own deterministic clock. Defined in the platform layer, not in pipeline.c.
uint32_t platform_now_ms(void);
