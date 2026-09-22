#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Board-specific GC9A01/SPI + LVGL display wiring (docs/OpenApex_SPEC.md board_profile.h pins).
// Owns the panel, the backlight, and the one lv_display_t the GUI layer draws into -- none of this
// may be referenced from firmware/gui, which only ever touches the LVGL display API generically
// (lv_display_get_default()).
//
// Must be called once, after lv_init() and before gui_app_init().
void display_driver_init(void);

#ifdef __cplusplus
}
#endif
