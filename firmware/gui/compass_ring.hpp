#pragma once

#include "lvgl.h"

// The compass ring shared by the turn-by-turn page and the odometer page: a full-diameter ring of
// tick marks with a red north marker riding around it.
//
// It lived inside NavRenderer first, because the nav dial was the only page that had one. The
// odometer page ("design reference/4. Odometer page") reuses the *same* element rather than drawing
// a lookalike, so it moved out here and NavRenderer now delegates to it -- one implementation, one
// set of constants, and a heading that cannot drift between the two pages.
//
// Draws straight into an LV_EVENT_DRAW_MAIN layer (see DialScreen::draw_event_cb) rather than owning
// widgets: 36 ticks would otherwise be 36 lv_obj_t plus a triangle, for something that never needs
// to be hit-tested or individually styled.
class CompassRing {
public:
    // `display_diameter_px` is the live panel diameter, not BOARD_DISP_WIDTH -- pages read it from
    // the active lv_display so one binary renders at any resolution.
    explicit CompassRing(int32_t display_diameter_px);

    // Screen-space heading the north mark points to (radians, 0 = up, clockwise). `known == false`
    // hides the marker entirely; the ticks still draw, so the ring never silently becomes a
    // fabricated "north is up".
    void set_north(float heading_rad, bool known);

    bool north_known() const { return north_known_; }

    // `coords` is the drawing area of the object being drawn into, as from lv_obj_get_coords().
    void draw(lv_layer_t *layer, const lv_area_t &coords) const;

private:
    const int32_t display_diameter_px_;
    float north_heading_rad_ = 0.0f;
    bool north_known_ = false;
};
