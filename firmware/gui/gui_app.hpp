#pragma once

#include "view_state.h"

#ifdef __cplusplus
extern "C" {
#endif

// Only entry points firmware/main/main.c (and sim_lvgl's main.cpp) may call into the GUI. Everything
// past this seam is C++ (Screen/GuiTheme classes, docs/OpenApex_SPEC.md §16.6); this header and
// terminal_view_state_t are the only things the C side needs to know.

// Builds the GUI's widget tree and loads the active screen. Call once, after LVGL and the display
// driver are initialized.
void gui_app_init(void);

// Updates the active screen to reflect `state`. Safe to call every frame; does not allocate.
void gui_app_update(const terminal_view_state_t *state);

#ifdef __cplusplus
}

class GuiTheme;

// The active theme for this build (BasicTheme or RichTheme, selected by BOARD_GFX_TIER). Screens
// call this for palette()/transition behaviour instead of hardcoding colours -- see §16.6: the
// theme owns the *how*, the screen owns the *what*. Valid before gui_app_init(); the theme is a
// stateless namespace-scope object.
const GuiTheme &gui_theme(void);
#endif
