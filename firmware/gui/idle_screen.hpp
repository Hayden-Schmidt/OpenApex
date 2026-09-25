#pragma once

#include "screen.hpp"

// design reference/2. Idle Screen/2. idle screen.md + "Idle Screen Ref.svg".
//
// The pre-navigation page: cached trip odometer in a theme-coloured bubble, phone link status, and
// -- once the phone is connected -- the wall clock and phone battery above it.
//
// Two layouts, tweened between over 1s on an ease-in-out path when the link state flips:
//   disconnected: bubble sits high and centred, bluetooth-disabled glyph below it, nothing above.
//   connected:    bubble and glyph slide down; clock + battery rise out from behind the bubble's
//                 top edge (clipped by `reveal_`, whose bottom edge tracks that edge).
class IdleScreen : public Screen {
public:
    IdleScreen();
    void update(const terminal_view_state_t &state) override;

private:
    // Positions every widget for a tween progress of 0 (disconnected) .. 1000 (connected).
    void apply_layout(int32_t progress);
    static void anim_exec_cb(void *var, int32_t value);

    // Reference-design pixels (the 240px profile the SVG is authored at) scaled to the live panel.
    int32_t px(float ref_240) const;

    float scale_ = 1.0f;
    int32_t width_ = 240;

    lv_obj_t *reveal_ = nullptr;        // clip window: screen top .. bubble top edge
    lv_obj_t *clock_label_ = nullptr;   // inside reveal_
    lv_obj_t *battery_icon_ = nullptr;  // inside reveal_
    lv_obj_t *bubble_ = nullptr;
    lv_obj_t *odometer_label_ = nullptr;  // inside bubble_
    lv_obj_t *link_icon_ = nullptr;

    int32_t progress_ = 0;  // 0..1000, current tween position
    bool connected_ = false;
    uint8_t last_battery_percent_ = 0xFE;  // != 0xFF so the first update always paints
    uint32_t last_odometer_meters_ = UINT32_MAX;
};
