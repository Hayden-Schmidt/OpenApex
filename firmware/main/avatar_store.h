#pragma once

#include <stdbool.h>
#include <stdint.h>

// The rider's avatar: an A8 alpha mask the size of the boot logo (BOARD_AVATAR_PX square), stored
// in its own `avatar` flash partition (partitions.csv) and tinted with the accent colour at draw
// time, exactly like the built-in logo. Survives reboots with no phone in range.
//
// Partition layout: a 16-byte header {magic, w, h, size, crc32} at offset 0, mask bytes from
// offset 16. The header is written last on commit, so a power cut mid-upload leaves no valid
// avatar rather than a half-written one.

// Reads the stored avatar into a new heap buffer (PSRAM when the heap has it) the caller frees.
// NULL when none is stored or it fails its CRC. *w/*h are set on success.
uint8_t *avatar_store_read(uint16_t *w, uint16_t *h);

// True once per successful commit since the last call. gui_task polls this to reload.
bool avatar_store_take_changed(void);

// Handles one write to the avatar upload characteristic (docs/OpenApex_SPEC.md §5.5). Returns 0 or
// a BLE ATT error code. Runs on the NimBLE host task.
int avatar_store_ble_write(const uint8_t *buf, uint16_t len);
