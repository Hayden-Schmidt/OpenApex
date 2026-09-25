#pragma once

#include <stdbool.h>
#include <stdint.h>

// Device odometer: distance ridden, integrated on the terminal from the relayed GNSS speed. Kept on
// the device (not the phone) so it counts whichever phone is relaying and survives without one.
// Host-testable -- no ESP-IDF dependency; odometer_store.c persists it across power cycles.

// Speeds below this are treated as standing still. GNSS speed at rest wanders by a km/h or two,
// which would otherwise accumulate into phantom distance over a long stop.
#define ODOMETER_MIN_SPEED_KMH 3.0f

// A gap longer than this between samples is not integrated across: the link or the fix dropped,
// and multiplying a stale speed by the whole gap would invent distance.
#define ODOMETER_MAX_GAP_MS 5000U

void odometer_reset(void);

// Feeds one GNSS speed sample. speed_valid/fix_valid false breaks the integration chain.
void odometer_accept(bool fix_valid, bool speed_valid, float speed_kmh, uint32_t now_ms);

// Whole metres ridden in total, including what was restored at boot.
uint32_t odometer_meters(void);

// Restores a persisted total (odometer_store.c). Replaces the current total.
void odometer_restore(uint32_t meters);
