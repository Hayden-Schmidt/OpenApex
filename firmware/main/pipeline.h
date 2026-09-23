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

// Advances countdown interpolation to now_ms and refreshes distance/stale in out.
void view_state_tick(uint32_t now_ms, terminal_view_state_t *out);

// Monotonic millisecond clock. On the ESP32 this wraps esp_timer_get_time(); host tests provide
// their own deterministic clock. Defined in the platform layer, not in pipeline.c.
uint32_t platform_now_ms(void);
