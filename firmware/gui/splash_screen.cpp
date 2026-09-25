#include "splash_screen.hpp"

#include "gui_app.hpp"
#include "icons.hpp"
#include "theme.hpp"

namespace {

// "Boot Screen.svg": black field, logo in a 70x70 box dead-centred on the 240px frame. The icon
// manifest already renders it at the right size for this panel (size_240 = 70), so the only
// geometry here is "centre it" -- no reference-pixel maths needed.
//
// The SVG fills the logo #D0D000, which is a Figma placeholder that contradicts the "default theme
// colour is not yellow" rule in DESIGN NOTES.md. The raster is an A8 mask, so the colour comes from
// the theme instead and the boot screen tracks the brand colour for free.
constexpr uint32_t kFadeInMs = 400;

} // namespace

SplashScreen::SplashScreen() {
    lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(root_, false);

    logo_ = lv_image_create(root_);
    lv_image_set_src(logo_, icon_image(ICON_LOGO));
    lv_obj_set_style_image_recolor(logo_, gui_theme().palette(), 0);
    lv_obj_set_style_image_recolor_opa(logo_, LV_OPA_COVER, 0);
    lv_obj_center(logo_);

    // The page doc allows a fade-in "maybe, if the C3 can handle it". It can: this is one whole-object
    // opacity value animated over a 70px image, which costs a blend on an area LVGL already redraws.
    // Same on both tiers -- the RICH tier's richer logo animation is deferred until there is a real
    // logo to animate.
    lv_obj_set_style_opa(logo_, LV_OPA_TRANSP, 0);
    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, logo_);
    lv_anim_set_values(&fade, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&fade, kFadeInMs);
    lv_anim_set_exec_cb(&fade, [](void *var, int32_t v) {
        lv_obj_set_style_opa(static_cast<lv_obj_t *>(var), static_cast<lv_opa_t>(v), 0);
    });
    lv_anim_start(&fade);
}

// Nothing on this page is state-driven: it shows the same thing regardless of link, navigation or
// battery, and gui_app.cpp decides when it stops being shown.
void SplashScreen::update(const terminal_view_state_t &state) { (void)state; }
