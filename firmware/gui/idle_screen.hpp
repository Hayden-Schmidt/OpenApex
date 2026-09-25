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
//
// Page entry (Screen Transitions.md) layers on top: the bubble rises in from below the panel, the
// glyph slides out from behind it, and only then does the connected layout play. Leaving reverses
// all three in turn.
class IdleScreen : public Screen {
public:
    IdleScreen();
    void update(const terminal_view_state_t &state) override;
    void enter() override;
    void leave(LeaveDone done, void *ctx) override;
    bool entered() const override;

private:
    // Positions every widget from progress_, bubble_in_ and link_in_.
    void apply_layout();
    static void set_progress(void *var, int32_t value);
    static void set_bubble_in(void *var, int32_t value);
    static void set_link_in(void *var, int32_t value);
    static IdleScreen *from_anim(lv_anim_t *a);
    void tween(lv_anim_exec_xcb_t exec, int32_t from, int32_t to, uint32_t full_ms,
               lv_anim_completed_cb_t completed,
               lv_anim_path_cb_t path = lv_anim_path_ease_in_out);
    // Tweens progress_ toward the current link state.
    void start_link_tween();

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

    int32_t progress_ = 0;   // 0..1000, disconnected .. connected layout
    int32_t bubble_in_ = 0;  // 0..1000, bubble below the panel .. at rest
    int32_t link_in_ = 0;    // 0..1000, glyph behind the bubble .. at rest
    bool landed_ = false;    // bubble and glyph entry finished
    bool leaving_ = false;
    LeaveDone done_ = nullptr;
    void *done_ctx_ = nullptr;
    int collapse_left_ = 0;  // leave(): text and glyph collapses still running
    bool connected_ = false;
    uint8_t last_battery_percent_ = 0xFE;  // != 0xFF so the first update always paints
    uint32_t last_odometer_meters_ = UINT32_MAX;
};
