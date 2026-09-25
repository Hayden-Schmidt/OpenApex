#pragma once

#include "lvgl.h"
#include "view_state.h"

// Upcoming-traffic ring for the turn-by-turn page -- the alternative to CompassRing in the same
// slot, selected at runtime (see gui_app_set_dial_outer).
//
// This is Google Maps' notification progress bar, bent around the screen. In the notification it is
// a straight line coloured in runs -- free-flowing, slowing, heavy, stopped -- across the rest of
// the trip. Here it is an arc running clockwise from the lower left, over the top, to the lower
// right, with a gap at the bottom that the ETA text sits in.
//
// The gap is not a mask. "OpenApex Hardware Design Ref.svg" cuts it with a black circle over the
// ring; the arc simply stops at the same two angles instead, which leaves clean ends rather than
// ones chopped square by a circle and costs nothing to draw.
//
// As the ride progresses the consumed part is dropped from the START, so the ring shortens toward
// the destination rather than filling up.
class TripArc {
public:
    explicit TripArc(int32_t display_diameter_px);

    // `state.traffic` / `traffic_count` / `trip_progress_permille`, straight from the view state.
    void set_state(const terminal_view_state_t &state);

    // True when there is anything to draw -- i.e. the phone actually supplied traffic runs. With no
    // data the ring is drawn as a single unknown-grey arc rather than vanishing, so the element
    // still reads as "the trip", not as a rendering failure.
    bool has_data() const { return count_ > 0; }

    void draw(lv_layer_t *layer, const lv_area_t &coords) const;

private:
    const int32_t display_diameter_px_;
    nav_traffic_span_t spans_[NAV_TRAFFIC_MAX_SPANS] = {};
    uint8_t count_ = 0;
    uint16_t progress_permille_ = 0;
};
