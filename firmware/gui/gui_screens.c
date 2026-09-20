#include "gui_screens.h"

#include "lvgl.h"

#include <stdio.h>

// Minimal first-cut terminal screen: a state banner, a big maneuver/distance readout, and a
// street-name line. Intentionally not the full 6-state SPEC layout yet (see docs/OpenApex_SPEC.md
// §9) -- this proves the view_state_t -> LVGL rendering pipeline end to end so later slices can
// flesh out per-state visuals without re-plumbing data flow.

static lv_obj_t *s_screen;
static lv_obj_t *s_state_label;
static lv_obj_t *s_readout_label;
static lv_obj_t *s_street_label;

static const char *icon_text(nav_icon_t icon) {
    switch (icon) {
        case NAV_ICON_STRAIGHT: return "STRAIGHT";
        case NAV_ICON_TURN_LEFT: return "TURN LEFT";
        case NAV_ICON_TURN_RIGHT: return "TURN RIGHT";
        case NAV_ICON_SLIGHT_LEFT: return "SLIGHT LEFT";
        case NAV_ICON_SLIGHT_RIGHT: return "SLIGHT RIGHT";
        case NAV_ICON_SHARP_LEFT: return "SHARP LEFT";
        case NAV_ICON_SHARP_RIGHT: return "SHARP RIGHT";
        case NAV_ICON_ROUNDABOUT: return "ROUNDABOUT";
        case NAV_ICON_U_TURN: return "U-TURN";
        case NAV_ICON_ARRIVED: return "ARRIVED";
        case NAV_ICON_UNKNOWN:
        default: return "--";
    }
}

static const char *state_text(view_state_t state) {
    switch (state) {
        case VIEW_IDLE: return "IDLE";
        case VIEW_ACTIVE: return "ACTIVE";
        case VIEW_STALE: return "STALE";
        case VIEW_ARRIVED: return "ARRIVED";
        default: return "?";
    }
}

static lv_color_t state_color(view_state_t state) {
    switch (state) {
        case VIEW_ACTIVE: return lv_color_hex(0x35C46B);
        case VIEW_STALE: return lv_color_hex(0xD9A441);
        case VIEW_ARRIVED: return lv_color_hex(0x4A90D9);
        case VIEW_IDLE:
        default: return lv_color_hex(0x808080);
    }
}

void gui_screens_init(void) {
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    s_state_label = lv_label_create(s_screen);
    lv_obj_set_style_text_color(s_state_label, lv_color_hex(0x808080), 0);
    lv_obj_align(s_state_label, LV_ALIGN_TOP_MID, 0, 20);
    lv_label_set_text(s_state_label, "IDLE");

    s_readout_label = lv_label_create(s_screen);
    lv_obj_set_style_text_color(s_readout_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_readout_label, &lv_font_montserrat_28, 0);
    lv_obj_align(s_readout_label, LV_ALIGN_CENTER, 0, -10);
    lv_label_set_text(s_readout_label, "--");

    s_street_label = lv_label_create(s_screen);
    lv_obj_set_style_text_color(s_street_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_align(s_street_label, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_label_set_text(s_street_label, "");

    lv_screen_load(s_screen);
}

void gui_screens_update(const terminal_view_state_t *state) {
    if (s_screen == NULL || state == NULL) {
        return;
    }

    lv_label_set_text(s_state_label, state_text(state->state));
    lv_obj_set_style_text_color(s_state_label, state_color(state->state), 0);

    char readout[48];
    if (state->state == VIEW_ACTIVE || state->state == VIEW_STALE) {
        snprintf(readout, sizeof(readout), "%s\n%um", icon_text(state->icon_type),
                 (unsigned)state->distance_meters);
    } else if (state->state == VIEW_ARRIVED) {
        snprintf(readout, sizeof(readout), "%s", icon_text(NAV_ICON_ARRIVED));
    } else {
        snprintf(readout, sizeof(readout), "--");
    }
    lv_label_set_text(s_readout_label, readout);

    lv_label_set_text(s_street_label, state->street_name[0] != '\0' ? state->street_name : "");
}
