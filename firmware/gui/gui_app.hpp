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

// Pages the rider selects, as opposed to states the navigation model puts us in. The odometer page
// (design reference/4. Odometer page) is not a view_state_t -- no packet or countdown result can
// ever produce it, it is chosen. GUI_PAGE_AUTO hands routing back to view_state_t.
//
// BACKEND GAP: nothing on the device calls this yet. Touch is live on the C3 (touch_driver.c), but
// its only gesture so far is the nav page's hold-to-swap ring; firmware/sim_lvgl's --page flag is
// the only caller today. This is the seam that page-switching input lands on.
typedef enum {
    GUI_PAGE_AUTO = 0,
    GUI_PAGE_ODOMETER,
} gui_page_override_t;

void gui_app_set_page_override(gui_page_override_t page);

// Which element rides around the outside of the turn-by-turn page: the compass ring, or the
// upcoming-traffic ring built from Google's notification progress bar (design reference/3.Turn by
// Turn). Swappable live -- both are the same slot, and the page keeps its arrow/street/distance
// either way.
//
// BACKEND GAP: as with the page override, nothing on the device calls this. How the rider picks
// (screen swipe, phone toggle) is undecided, and the C3 has no input hardware modelled for it.
typedef enum {
    GUI_DIAL_OUTER_COMPASS = 0,
    GUI_DIAL_OUTER_TRIP_ARC,
} gui_dial_outer_t;

void gui_app_set_dial_outer(gui_dial_outer_t outer);

// How the turn-by-turn page comes on screen ("design reference/Screen Transitions.md"): each
// element animating in on its own (arrow, text, ring), or the whole page fading in. Both exist so
// they can be compared on the panel; the loser goes once one is picked.
//
// BACKEND GAP: as above -- only firmware/sim_lvgl's --nav-entry flag sets this today.
typedef enum {
    GUI_NAV_ENTRY_ELEMENTS = 0,
    GUI_NAV_ENTRY_FADE,
} gui_nav_entry_t;

void gui_app_set_nav_entry(gui_nav_entry_t entry);
gui_nav_entry_t gui_app_nav_entry(void);

#ifdef __cplusplus
}

class GuiTheme;

// The active theme for this build (BasicTheme or RichTheme, selected by BOARD_GFX_TIER). Screens
// call this for palette()/transition behaviour instead of hardcoding colours -- see §16.6: the
// theme owns the *how*, the screen owns the *what*. Valid before gui_app_init(); the theme is a
// stateless namespace-scope object.
const GuiTheme &gui_theme(void);
#endif
