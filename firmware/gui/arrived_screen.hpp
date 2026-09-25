#pragma once

#include "screen.hpp"

// design reference/3.Turn by Turn/Arrived Pop Up.svg -- the VIEW_ARRIVED state.
//
// A full-bleed green surface with a centred destination pin. It gets its own Screen rather than
// living as a special case inside DialScreen because it shares nothing with the dial: no compass,
// no maneuver, no distance, and a background that covers the whole panel.
//
// The reference calls this a "pop up": it is a full screen (the green circle fills the whole 240
// frame) that animates in OVER the dial. The green closes in from the panel edge as a shrinking
// circular hole, revealing the pin last (enter()).
class ArrivedScreen : public Screen {
public:
    ArrivedScreen();
    void update(const terminal_view_state_t &state) override;
    void enter() override;
    void leave(LeaveDone done, void *ctx) override;
    bool entered() const override { return overlay_ == nullptr; }
    bool enters_over() const override { return true; }

private:
    static void overlay_draw_cb(lv_event_t *e);
    void end_reveal();

    lv_obj_t *pin_ = nullptr;
    lv_obj_t *overlay_ = nullptr;  // on lv_layer_top() while the reveal runs
    int32_t cover_radius_ = 0;     // px, centre to the panel's corners
    int32_t hole_radius_ = 0;      // px, the part of the previous page still showing
};
