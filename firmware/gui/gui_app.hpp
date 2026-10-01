#pragma once

#include "view_state.h"

#ifdef __cplusplus
extern "C" {
#endif

// Only entry points firmware/main/main.c (and sim_lvgl's main.cpp) may call into the GUI. Everything
// past this seam is C++ (Screen/GuiTheme classes, docs/reference/gui_and_display.md); this header and
// terminal_view_state_t are the only things the C side needs to know.

// Builds the GUI's widget tree and loads the active screen. Call once, after LVGL and the display
// driver are initialized.
void gui_app_init(void);

// Updates the active screen to reflect `state`. Safe to call every frame; does not allocate.
void gui_app_update(const terminal_view_state_t *state);

// Pages the rider selects, as opposed to states the navigation model puts us in. The odometer page
// (design/screens/04-odometer) is not a view_state_t -- no packet or countdown result can
// ever produce it, it is chosen. GUI_PAGE_AUTO hands routing back to view_state_t.
//
// The device reaches the odometer page by double tap and by speed (gui_app.cpp); firmware/sim_lvgl's
// --page flag sets it directly.
typedef enum {
    GUI_PAGE_AUTO = 0,
    GUI_PAGE_ODOMETER,
} gui_page_override_t;

void gui_app_set_page_override(gui_page_override_t page);

// Which element rides around the outside of the turn-by-turn page: the compass ring, or the
// upcoming-traffic ring built from Google's notification progress bar
// (design/screens/03-turn-by-turn). Swappable live -- both are the same slot, and the page
// keeps its arrow/street/distance either way.
//
// BACKEND GAP: as with the page override, nothing on the device calls this. How the rider picks
// (screen swipe, phone toggle) is undecided, and the C3 has no input hardware modelled for it.
typedef enum {
    GUI_DIAL_OUTER_COMPASS = 0,
    GUI_DIAL_OUTER_TRIP_ARC,
} gui_dial_outer_t;

void gui_app_set_dial_outer(gui_dial_outer_t outer);

// How the turn-by-turn page comes on screen ("design/SCREEN_TRANSITIONS.md"): each
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

// Called on gui_task when the rider taps Accept on the incoming-call page, with the view state's
// call_id. The GUI never touches BLE (§346): main.c queues it for countdown_task to send, the
// simulator just logs it. Unset = the page shows the caller with no button.
typedef void (*gui_call_accept_cb_t)(uint16_t call_id);
void gui_app_set_call_accept_handler(gui_call_accept_cb_t cb);

// Add friend (docs/blueprints/04_rider_mesh.md U1-U5). The GUI owns whether the menu is open: a
// finger held still for 5 s on any page but a call opens it (not in or near a turn:
// terminal_view_state_t::friend_menu_allowed; the hold is cancelled once it opens, so it cannot run
// on into the friend ring), an incoming request opens it, and it closes on a double tap, 3 s after
// "Added!", when a request that opened it lapses, or after a minute with nothing happening.
//
// Double tap elsewhere toggles the odometer page from the idle or nav page (over navigation only
// until a turn is near). With setting 0x0A on, riding over 5 km/h with no navigation brings the
// odometer page up and 3 min stopped takes it back to idle. Both callbacks run on gui_task; as with
// calls, main.c hands them to the mesh task and the GUI never touches a radio.
//   hold: the rider filled the ring on a rider's card (mesh_hold_friend).
//   menu: Add friend opened or closed (mesh_set_menu_open).
typedef void (*gui_friend_hold_cb_t)(const uint8_t *rider_id);
typedef void (*gui_friend_menu_cb_t)(bool open);
void gui_app_set_friend_handlers(gui_friend_hold_cb_t hold, gui_friend_menu_cb_t menu);

// Opens Add friend as a double tap would (simulator, dev console), on the next frame. Safe from any
// task. Ignored while the gate is closed.
void gui_app_open_friend_menu(void);

// The rider's uploaded avatar (firmware/main/avatar_store.h): GUI_AVATAR_BYTES of A2 art, 16x16
// then 8x8 (the 8x8 is not drawn yet), shown in place of the boot logo in the accent.
// Copied, so the caller's buffer may go. NULL clears it. Call before gui_app_init() to have it on
// the first frame; later calls take effect immediately.
#define GUI_AVATAR_BYTES 80
void gui_app_set_avatar(const uint8_t *a2);

// The uploaded avatar set above, or NULL. For drawing our own character on other screens.
const uint8_t *gui_app_custom_avatar(void);

#ifdef __cplusplus
}

#include "lvgl.h"

class GuiTheme;

// The active theme for this build (BasicTheme or RichTheme, selected by BOARD_GFX_TIER). Screens
// call this for palette()/transition behaviour instead of hardcoding colours -- see docs/reference/gui_and_display.md: the
// theme owns the *how*, the screen owns the *what*. Valid before gui_app_init(); the theme is a
// stateless namespace-scope object.
const GuiTheme &gui_theme(void);

// Shared style carrying the accent as text colour, background colour and image recolour. An object
// that wears the theme colour for its whole life adds this instead of setting the colour locally,
// so a colour change from the phone restyles every page with one lv_obj_report_style_change().
lv_style_t *gui_accent_style(void);

// Image source for the boot logo: the selected preset or uploaded avatar, else the default character.
// It is AVATAR_ART_PX square at 1x; the caller scales it to BOARD_AVATAR_PX (nearest-neighbour).
const void *gui_logo_src(void);
#endif
