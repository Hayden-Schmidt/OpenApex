#include "arrived_screen.hpp"

#include <algorithm>
#include <cmath>

#include "ease.hpp"
#include "gui_app.hpp"
#include "icons.hpp"
#include "theme.hpp"

namespace {

// The SVG centres the pin's ink at (120.33, 119.92) on the 240 frame -- i.e. dead centre to within
// a third of a pixel, so lv_obj_center() is the honest expression of it rather than a hand-offset.
// Its 36.67x45.83 ink sets the icon's 54px box (see icons_manifest.json's note on location_on).
constexpr uint32_t kPinColor = 0xE3E3E3;

// The reveal: a green ring closing in from the panel edge to the centre over the nav page, pin
// last. Exponential ease-in, so it creeps at first and accelerates as it closes.
constexpr uint32_t kRevealMs = 2000;
// Pin rows are clipped in bands this tall while the hole crosses it -- coarse enough to keep the
// draw count down, fine enough that the stepped edge is not visible at the speed it moves.
constexpr int32_t kPinBandPx = 2;

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

// The previous page is still loaded (enters_over()), so the reveal is drawn over it from the top
// layer, and this page's own root is only loaded once the ring has closed -- at which point the two
// are the same picture and the swap is invisible.
void ArrivedScreen::enter() {
    if (lv_screen_active() == root_) return;  // first page on boot: nothing to reveal over

    lv_display_t *disp = lv_display_get_default();
    const int32_t w = lv_display_get_horizontal_resolution(disp);
    const int32_t h = lv_display_get_vertical_resolution(disp);
    // Out to the corners, not just the round panel's edge, so a square simulator window is covered
    // too.
    cover_radius_ = static_cast<int32_t>(std::ceil(std::sqrt(static_cast<float>(w * w + h * h)) / 2.0f));
    hole_radius_ = cover_radius_;

    overlay_ = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(overlay_);
    lv_obj_set_size(overlay_, w, h);
    lv_obj_set_pos(overlay_, 0, 0);
    lv_obj_add_event_cb(overlay_, overlay_draw_cb, LV_EVENT_DRAW_MAIN, this);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, this);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, kRevealMs);
    lv_anim_set_path_cb(&a, anim_path<ease_in_expo>);
    lv_anim_set_exec_cb(&a, [](void *var, int32_t v) {
        auto *self = static_cast<ArrivedScreen *>(var);
        self->hole_radius_ = self->cover_radius_ * (1000 - v) / 1000;
        lv_obj_invalidate(self->overlay_);
    });
    lv_anim_set_completed_cb(&a, [](lv_anim_t *done) {
        auto *self = static_cast<ArrivedScreen *>(done->var);
        lv_screen_load(self->root_);
        self->end_reveal();
    });
    lv_anim_start(&a);
}

void ArrivedScreen::end_reveal() {
    lv_anim_delete(this, nullptr);
    if (overlay_ != nullptr) {
        lv_obj_delete(overlay_);
        overlay_ = nullptr;
    }
}

// Leaving mid-reveal must drop the overlay, or it would keep closing over the next page and then
// load this one on top of it.
void ArrivedScreen::leave(LeaveDone done, void *ctx) {
    end_reveal();
    done(ctx);
}

void ArrivedScreen::overlay_draw_cb(lv_event_t *e) {
    auto *self = static_cast<ArrivedScreen *>(lv_event_get_user_data(e));
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t coords;
    lv_obj_get_coords(static_cast<lv_obj_t *>(lv_event_get_target(e)), &coords);
    const int32_t cx = (coords.x1 + coords.x2 + 1) / 2;
    const int32_t cy = (coords.y1 + coords.y2 + 1) / 2;
    const int32_t r = self->hole_radius_;

    // The green annulus: LVGL's arc radius is the outer edge and the stroke grows inward, so a
    // stroke of (outer - hole) leaves exactly the hole open.
    if (r < self->cover_radius_) {
        lv_draw_arc_dsc_t arc;
        lv_draw_arc_dsc_init(&arc);
        arc.center = {cx, cy};
        arc.radius = self->cover_radius_;
        arc.width = self->cover_radius_ - r;
        arc.start_angle = 0;
        arc.end_angle = 360;
        arc.color = gui_theme().arrived_surface();
        arc.opa = LV_OPA_COVER;
        lv_draw_arc(layer, &arc);
    }

    // The pin sits under the same mask: only the part outside the hole shows. LVGL has no inverse
    // circular clip, so the pin is drawn in thin horizontal bands, each clipped to the chord of the
    // band that lies outside the hole -- one or two rectangles per band.
    const lv_image_dsc_t *pin = icon_image(ICON_LOCATION_ON);
    const int32_t pw = static_cast<int32_t>(pin->header.w);
    const int32_t ph = static_cast<int32_t>(pin->header.h);
    const float corner = std::sqrt(static_cast<float>(pw * pw + ph * ph)) / 2.0f;
    if (static_cast<float>(r) >= corner) return;  // hole still covers the whole pin

    lv_area_t pin_area;
    pin_area.x1 = cx - pw / 2;
    pin_area.y1 = cy - ph / 2;
    pin_area.x2 = pin_area.x1 + pw - 1;
    pin_area.y2 = pin_area.y1 + ph - 1;

    lv_draw_image_dsc_t img;
    lv_draw_image_dsc_init(&img);
    img.src = pin;
    img.recolor = lv_color_hex(kPinColor);
    img.recolor_opa = LV_OPA_COVER;

    if (r <= 0) {
        lv_draw_image(layer, &img, &pin_area);
        return;
    }

    const lv_area_t saved_clip = layer->_clip_area;
    const auto draw_clipped = [&](int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
        lv_area_t band = {std::max(x1, saved_clip.x1), std::max(y1, saved_clip.y1),
                          std::min(x2, saved_clip.x2), std::min(y2, saved_clip.y2)};
        if (band.x1 > band.x2 || band.y1 > band.y2) return;
        layer->_clip_area = band;
        lv_draw_image(layer, &img, &pin_area);
    };
    for (int32_t y = pin_area.y1; y <= pin_area.y2; y += kPinBandPx) {
        const int32_t y2 = std::min(y + kPinBandPx - 1, pin_area.y2);
        // The band's row nearest the centre decides its chord, so nothing inside the hole leaks.
        const int32_t dy = (y2 < cy) ? cy - y2 : (y > cy ? y - cy : 0);
        if (dy >= r) {
            draw_clipped(pin_area.x1, y, pin_area.x2, y2);
            continue;
        }
        const int32_t half_chord =
            static_cast<int32_t>(std::ceil(std::sqrt(static_cast<float>(r * r - dy * dy))));
        draw_clipped(pin_area.x1, y, cx - half_chord - 1, y2);
        draw_clipped(cx + half_chord, y, pin_area.x2, y2);
    }
    layer->_clip_area = saved_clip;
}
