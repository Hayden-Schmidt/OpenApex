#pragma once

#include <stdint.h>

// NVS persistence for settings.h. Stored as the same TLV the phone writes, so a firmware update
// that adds a tag reads an old blob as "old fields set, new fields default" with no migration.

// Loads the saved settings into settings_set(). Requires NVS to be initialised. Called on the boot
// path before gui_task starts, so the splash already shows the rider's colour and avatar with no
// phone in range.
void settings_store_load(void);

// Writes the current settings once they have been stable for a short quiet period, so a colour
// slider dragged on the phone costs one flash write rather than dozens. Cheap to call every tick.
void settings_store_maybe_save(uint32_t now_ms);
