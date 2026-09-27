#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "host/ble_gatt.h"

// The rider's uploaded avatar (docs/OpenApex_SPEC.md §5.5): colour-agnostic A2 art, a 16x16 and an
// 8x8 version back to back, tinted with the accent colour at draw time like the built-in presets.
// 80 bytes, so it lives in NVS beside the settings and survives reboots with no phone in range.

#define AVATAR_STORE_BYTES 80u

// Copies the stored avatar into `out`. False when none is stored.
bool avatar_store_read(uint8_t out[AVATAR_STORE_BYTES]);

// True once per change (upload or clear) since the last call. gui_task polls this to reload.
bool avatar_store_take_changed(void);

// Access callback body for the avatar characteristic: a read returns the stored 80 bytes (empty
// when none), a write of exactly 80 bytes replaces it and an empty write clears it. Runs on the
// NimBLE host task; returns 0 or a BLE ATT error code.
int avatar_store_ble_access(struct ble_gatt_access_ctxt *ctxt);
