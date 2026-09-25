#include "arrived_screen.hpp"

#include <cmath>

#include "gui_app.hpp"
#include "icons.hpp"
#include "theme.hpp"

namespace {

// The SVG centres the pin's ink at (120.33, 119.92) on the 240 frame -- i.e. dead centre to within
// a third of a pixel, so lv_obj_center() is the honest expression of it rather than a hand-offset.
// Its 36.67x45.83 ink sets the icon's 54px box (see icons_manifest.json's note on location_on).
constexpr uint32_t kPinColor = 0xE3E3E3;

} // namespace

ArrivedScreen::ArrivedScreen() {
    lv_obj_set_style_bg_color(root_, gui_theme().arrived_surface(), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(root_, false);

    pin_ = lv_image_create(root_);
    lv_image_set_src(pin_, icon_image(ICON_LOCATION_ON));
    lv_obj_set_style_image_recolor(pin_, lv_color_hex(kPinColor), 0);
    lv_obj_set_style_image_recolor_opa(pin_, LV_OPA_COVER, 0);
    lv_obj_center(pin_);
}

// Nothing here is state-driven: arriving is arriving. gui_app.cpp decides when it is shown.
void ArrivedScreen::update(const terminal_view_state_t &state) { (void)state; }
