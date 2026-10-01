#pragma once

#include "compass_ring.hpp"
#include "screen.hpp"

// design/screens/04-odometer/NOTES.md + "Odometer page ref.svg".
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
    // As the nav page ("design/SCREEN_TRANSITIONS.md"): the text slides up from the bottom, then the
    // compass ring zooms in from beyond the edge. Leaving plays it backwards.
    void enter() override;
    void leave(LeaveDone done, void *ctx) override;
    bool entered() const override { return landed_; }

private:
    static void draw_event_cb(lv_event_t *e);
    static void set_text_in(void *var, int32_t value);
    static void set_ring_in(void *var, int32_t value);
    void run(lv_anim_exec_xcb_t exec, int32_t from, int32_t to, uint32_t delay_ms, uint32_t ms,
             lv_anim_path_cb_t path, lv_anim_completed_cb_t completed);

    int32_t text_in_ = 1000;  // 0..1000: off the bottom .. in place
    int32_t ring_in_ = 1000;  // 0..1000: oversized past the edge .. at rest
    bool landed_ = true;
    LeaveDone done_ = nullptr;
    void *done_ctx_ = nullptr;

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
    bool last_imperial_ = false;
    char last_clock_[NAV_CLOCK_LEN] = {0};
};
