#pragma once

// Shared LVGL screen-building code for the OpenApex terminal display. Written as plain LVGL C so
// the same translation unit can be compiled into both the hardware-less SDL simulator
// (firmware/sim_lvgl) and the real on-device gui_task (firmware/main/main.c) once Slice D wires up
// the physical GC9A01 panel. This file must only depend on LVGL and view_state.h — no SDL, no
// FreeRTOS, no board_profile.h pin definitions.
//
// Usage:
//   gui_screens_init();                          // once, after lv_init() and display creation
//   gui_screens_update(&frame);                  // every frame, frame from view_state_snapshot()

#include "view_state.h"

#ifdef __cplusplus
extern "C" {
#endif

// Builds the (currently single) terminal screen's LVGL widget tree and loads it as the active
// screen. Must be called once, after LVGL and the display driver are initialized.
void gui_screens_init(void);

// Updates the already-built widget tree in place to reflect `state`. Safe to call every frame;
// does not allocate or rebuild widgets.
void gui_screens_update(const terminal_view_state_t *state);

#ifdef __cplusplus
}
#endif
