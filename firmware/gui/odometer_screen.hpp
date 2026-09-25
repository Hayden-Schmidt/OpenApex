#pragma once

#include "compass_ring.hpp"
#include "screen.hpp"

// design reference/4. Odometer page/odometer page.md + "Odometer page ref.svg".
//
// Lifetime distance ridden with OpenApex, over the shared compass ring. Top to bottom: the heading
// readout (degrees + cardinal, theme colour), the odometer's boxed digits, the wall clock, and the
// page-position dots.
//
// The compass ring is the SAME element the turn-by-turn page draws (compass_ring.hpp), not a
// lookalike -- the page doc calls for reuse, so extracting it out of NavRenderer was part of
// building this page.
class OdometerScreen : public Screen {
public:
    OdometerScreen();
    void update(const terminal_view_state_t &state) override;

private:
    static void draw_event_cb(lv_event_t *e);

    // Reference-design pixels (the 240px frame the SVG is authored at) scaled to the live panel.
    int32_t px(float ref_240) const;

    // Repositions the centre-anchored text from its measured size.
    void place_text();

    // Writes kDigitCount digits, most significant first.
    static void odometer_digits(uint32_t meters, char *out);

    float scale_ = 1.0f;
    int32_t width_ = 240;

    CompassRing compass_;
    lv_obj_t *compass_obj_ = nullptr;  // LV_EVENT_DRAW_MAIN surface for compass_

    lv_obj_t *heading_label_ = nullptr;   // "330°"
    lv_obj_t *cardinal_label_ = nullptr;  // "NW"

    // Three full-size digit boxes plus a smaller tenths box; see kDigitCount in the .cpp for why
    // the count follows the SVG rather than the prose in the page doc.
    static constexpr int kDigitCount = 4;
    lv_obj_t *digit_box_[kDigitCount] = {};
    lv_obj_t *digit_label_[kDigitCount] = {};
    lv_obj_t *km_label_ = nullptr;

    lv_obj_t *clock_label_ = nullptr;
    lv_obj_t *dot_[2] = {};

    // Last rendered values, so update() only touches LVGL when something actually changed.
    uint16_t last_heading_deg_ = 0xFFFE;
    uint32_t last_odometer_meters_ = UINT32_MAX;
    char last_clock_[NAV_CLOCK_LEN] = {0};
};
