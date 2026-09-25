#pragma once

#include "gui_app.hpp"
#include "nav_renderer.hpp"
#include "trip_arc.hpp"
#include "screen.hpp"

// Turn-by-turn dial: a NavRenderer-driven maneuver animation (route/camera/tween/compass ring,
// ported from Demo/demo.js) plus a distance label. ARRIVED/UNKNOWN have no NavRenderer geometry
// (Demo/Google Icons Archive never modeled them) and get a small dedicated glyph instead -- see
// dial_screen.cpp.
class DialScreen : public Screen {
public:
    DialScreen();
    void update(const terminal_view_state_t &state) override;
    void enter() override;

    // Swaps the outer element. Safe to call at any time; takes effect on the next frame.
    void set_outer(gui_dial_outer_t outer);

private:
    lv_obj_t *canvas_obj_;      // custom-draw obj: NavRenderer draws into its LV_EVENT_DRAW_MAIN layer
    lv_obj_t *distance_label_;
    lv_obj_t *street_label_;   // street_name; empty when the maps app supplies none
    lv_obj_t *eta_label_;      // eta; same
    lv_obj_t *status_shaft_;    // ARRIVED/UNKNOWN glyph line 1 (hidden otherwise)
    lv_obj_t *status_head_;     // ARRIVED/UNKNOWN glyph line 2 (hidden otherwise)

    NavRenderer renderer_;
    TripArc trip_arc_;
    gui_dial_outer_t outer_ = GUI_DIAL_OUTER_COMPASS;
    int32_t status_box_size_; // runtime display resolution, not BOARD_DISP_WIDTH -- see .cpp
    nav_icon_t last_icon_ = static_cast<nav_icon_t>(-1); // forces add_maneuver on first update()
    uint16_t last_heading_deg_ = 0xFFFFu;

    // Page entry state (see enter()).
    bool arrow_entry_pending_ = false;  // next drawable maneuver runs on via enter_maneuver()
    int32_t ring_in_ = 1000;            // 0..1000, oversized .. at rest
    float ring_scale() const;
    static void set_ring_in(void *var, int32_t value);
    // Test control: a 2 s press-and-hold swaps the compass ring for the Maps trip arc and back.
    static void on_long_press(lv_event_t *e);
    static void set_swap_in(void *var, int32_t value);  // ring zoom + ETA slide, for the swap
    bool outer_swapping_ = false;       // outgoing ring is zooming out; ignore further holds
    static void set_text_in(void *var, int32_t value);
    static void set_page_opa(void *var, int32_t value);
    void tween(lv_anim_exec_xcb_t exec, uint32_t delay_ms, uint32_t ms, lv_anim_path_cb_t path);

    // Sized for the ARRIVED badge's ring, which is the longer of the two glyphs: one point every
    // kStatusRingStepDeg from 0 to 360 inclusive. The UNKNOWN glyph needs only 4.
    static constexpr float kStatusRingStepDeg = 24.0f;
    static constexpr int kStatusShaftMaxPts = 360 / 24 + 1;
    lv_point_precise_t status_shaft_pts_[kStatusShaftMaxPts];
    lv_point_precise_t status_head_pts_[2];

    static void draw_event_cb(lv_event_t *e);
    void draw_status_glyph(nav_icon_t icon);
};
