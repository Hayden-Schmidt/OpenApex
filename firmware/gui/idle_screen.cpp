#include "idle_screen.hpp"

IdleScreen::IdleScreen() {
    lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
}

void IdleScreen::update(const terminal_view_state_t &state) {
    (void)state;
    // TODO: phone/connection-status icon, time display -- design reference/2. Idle Screen.
}
