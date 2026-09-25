#pragma once

// Simulator-only screenshot support (see src/main.cpp's --shot flag). Not compiled into firmware.

#include <cstdint>

// Snapshots the currently active LVGL screen and writes it to `path` as a PNG, overwriting any
// existing file. Returns false (and prints to stderr) on failure.
bool shot_write_active_screen(const char *path);
