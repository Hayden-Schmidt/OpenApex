#pragma once

#include "screen.hpp"

// design reference/3.Turn by Turn/Arrived Pop Up.svg -- the VIEW_ARRIVED state.
//
// A full-bleed green surface with a centred destination pin. It gets its own Screen rather than
// living as a special case inside DialScreen because it shares nothing with the dial: no compass,
// no maneuver, no distance, and a background that covers the whole panel.
//
// NOTE the reference calls this a "pop up". It is built here as a full screen, which is what the
// SVG draws (the green circle fills the entire 240 frame). If it is meant to animate IN over the
// dial rather than replace it, that is a transition -- GuiTheme::apply_state_change is the hook,
// and it is still an instant lv_screen_load on both tiers. Flagged in the page doc.
class ArrivedScreen : public Screen {
public:
    ArrivedScreen();
    void update(const terminal_view_state_t &state) override;

private:
    lv_obj_t *pin_ = nullptr;
};
