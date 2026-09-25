#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Board-specific GC9A01/SPI + LVGL display wiring (docs/OpenApex_SPEC.md board_profile.h pins).
// Owns the panel, the backlight, and the one lv_display_t the GUI layer draws into -- none of this
// may be referenced from firmware/gui, which only ever touches the LVGL display API generically
// (lv_display_get_default()).
//
// Must be called once, after lv_init() and before gui_app_init(). Leaves the backlight OFF -- see
// display_driver_backlight_on().
// Drives the backlight off. Called first thing in app_main, well before display_driver_init(), so
// the panel stays dark through the rest of boot instead of lighting a stale GRAM frame.
void display_driver_hold_dark(void);

void display_driver_init(void);

// Turns the backlight on. Deliberately separate from display_driver_init(): the GC9A01 powers up
// with uninitialized GRAM, so lighting the panel before the first frame has been flushed shows the
// rider a burst of noise ahead of the boot logo. Call this once, immediately after the first
// lv_timer_handler() has drawn a frame.
void display_driver_backlight_on(void);

#ifdef __cplusplus
}
#endif
