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
constexpr uint32_t kFadeMs = 400;

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
    fade(LV_OPA_COVER, nullptr);
}

void SplashScreen::fade(lv_opa_t to, lv_anim_completed_cb_t completed) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, logo_);
    lv_anim_set_values(&a, lv_obj_get_style_opa(logo_, LV_PART_MAIN), to);
    lv_anim_set_duration(&a, kFadeMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, [](void *var, int32_t v) {
        lv_obj_set_style_opa(static_cast<lv_obj_t *>(var), static_cast<lv_opa_t>(v), 0);
    });
    lv_anim_set_user_data(&a, this);
    lv_anim_set_completed_cb(&a, completed);
    lv_anim_start(&a);
}

// "Screen Transitions.md": the splash leaves by fading out on the same curve it faded in on -- no
// scaling or sliding.
void SplashScreen::leave(LeaveDone done, void *ctx) {
    done_ = done;
    done_ctx_ = ctx;
    fade(LV_OPA_TRANSP, [](lv_anim_t *a) {
        auto *self = static_cast<SplashScreen *>(lv_anim_get_user_data(a));
        self->done_(self->done_ctx_);
    });
}

// Nothing on this page is state-driven: it shows the same thing regardless of link, navigation or
// battery, and gui_app.cpp decides when it stops being shown.
void SplashScreen::update(const terminal_view_state_t &state) { (void)state; }
