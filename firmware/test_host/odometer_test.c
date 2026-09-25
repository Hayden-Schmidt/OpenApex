#include <assert.h>
#include <stdio.h>

#include "../main/odometer.h"

int main(void) {
    // Steady 36 km/h (10 m/s) sampled at 1 Hz for 10 s = 100 m.
    odometer_reset();
    for (uint32_t t = 0; t <= 10000; t += 1000) {
        odometer_accept(true, true, 36.0f, t);
    }
    assert(odometer_meters() == 100);

    // Standing still: GNSS speed jitter below the threshold adds nothing beyond the 5 m of the
    // 10 m/s -> 0 slow-down second (trapezoid).
    for (uint32_t t = 11000; t <= 71000; t += 1000) {
        odometer_accept(true, true, (t / 1000) % 2 ? 2.5f : 1.0f, t);
    }
    assert(odometer_meters() == 105);

    // A gap past ODOMETER_MAX_GAP_MS is not integrated across.
    odometer_reset();
    odometer_accept(true, true, 36.0f, 0);
    odometer_accept(true, true, 36.0f, 60000);
    assert(odometer_meters() == 0);

    // Losing the fix breaks the chain: no distance across the fix-less stretch.
    odometer_reset();
    odometer_accept(true, true, 36.0f, 0);
    odometer_accept(false, true, 36.0f, 1000);
    odometer_accept(true, true, 36.0f, 2000);
    assert(odometer_meters() == 0);
    odometer_accept(true, true, 36.0f, 3000);
    assert(odometer_meters() == 10);

    // Restore carries a persisted total forward.
    odometer_restore(123456);
    odometer_accept(true, true, 36.0f, 4000);
    assert(odometer_meters() == 123466);

    puts("odometer tests passed");
    return 0;
}
