#pragma once

#include <stdint.h>

// Cross-boot persistence for heading_fusion.c's state, backed by NVS (the same partition
// ble_link_init() already initializes for BLE bonding). See docs/Heading_Sensor_Fusion_Plan.md,
// "cross-boot persistence": ignition-off cuts ESP power outright, so without this the mount-offset
// estimator would reconverge from scratch every ride. Kept out of heading_fusion.c itself so that
// module stays host-testable with no ESP-IDF dependency.

// Reads the cached state, if any, and feeds it to heading_fusion_restore_state(). Safe to call when
// nothing has ever been saved (first boot, or a fresh NVS partition) -- heading_fusion simply stays
// cold, same as after heading_fusion_reset(). Call once at boot, after NVS is initialized
// (ble_link_init()) and before the countdown task starts decoding packets.
void heading_store_load(void);

// Writes the current heading_fusion state to NVS, throttled to roughly once every 30s and skipped
// entirely while heading_fusion has nothing to save yet. Self-throttling, so the caller can just
// call this on every tick (e.g. countdown_task's existing 100ms loop) without its own cadence logic
// or flash-wear bookkeeping.
void heading_store_maybe_save(uint32_t now_ms);
