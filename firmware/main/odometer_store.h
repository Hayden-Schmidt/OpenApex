#pragma once

#include <stdint.h>

// Cross-boot persistence for odometer.c, in the NVS partition ble_link_init() brings up. Mirrors
// heading_store.{h,c}: kept separate so odometer.c stays host-testable.

// Restores the saved total, if any. Call once at boot, after ble_link_init() and before
// countdown_task starts feeding packets.
void odometer_store_load(void);

// Writes the total when it has advanced by at least 100 m and 30 s have passed since the last
// write. Self-throttling -- call on every countdown_task tick. Ignition-off cuts power without
// warning, so at most that much distance is lost per ride.
void odometer_store_maybe_save(uint32_t now_ms);
