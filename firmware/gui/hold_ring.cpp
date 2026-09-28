#include "hold_ring.hpp"

#include <cmath>
#include <initializer_list>

#include "ease.hpp"

namespace {

// Gradient ring as short arcs, each one flat colour. 5 degrees is below what the eye resolves as
// banding at this stroke width; each overlaps the next by a degree so anti-aliased ends leave no seam.
constexpr int kSegmentDeg = 5;

lv_color_t gradient_at(lv_color_t left, lv_color_t right, float deg_from_top) {
    // x across the panel, -1 at nine o'clock .. +1 at three; black where x is 0.
    const float x = std::sin(deg_from_top * 3.14159265f / 180.0f);
    const lv_opa_t mix = static_cast<lv_opa_t>(std::lround(std::fabs(x) * 255.0f));
    return lv_color_mix(x < 0.0f ? left : right, lv_color_black(), mix);
}

} // namespace

void HoldRing::create(lv_obj_t *parent, int32_t diameter, int32_t stroke) {
    stroke_ = stroke;
    obj_ = lv_obj_create(parent);
    lv_obj_remove_style_all(obj_);
    lv_obj_set_size(obj_, diameter, diameter);
    lv_obj_set_pos(obj_, 0, 0);
    lv_obj_remove_flag(obj_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(obj_, draw_cb, LV_EVENT_DRAW_MAIN, this);
}

void HoldRing::set_color(lv_color_t color) {
    left_ = right_ = color;
    gradient_ = false;
    lv_obj_invalidate(obj_);
}

void HoldRing::set_gradient(lv_color_t left, lv_color_t right) {
    left_ = left;
    right_ = right;
    gradient_ = true;
    lv_obj_invalidate(obj_);
}

void HoldRing::exec(void *var, int32_t value) {
    auto *self = static_cast<HoldRing *>(var);
    self->t_ = value;
    lv_obj_invalidate(self->obj_);
}

void HoldRing::completed(lv_anim_t *a) {
    auto *self = static_cast<HoldRing *>(a->var);
    const Done done = self->done_;
    self->done_ = nullptr;
    if (done != nullptr) done(self->done_ctx_);
}

void HoldRing::run(int32_t to, Done done, void *ctx) {
    lv_anim_delete(this, exec);
    done_ = done;
    done_ctx_ = ctx;
    const int32_t span = to > t_ ? to - t_ : t_ - to;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, this);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_values(&a, t_, to);
    lv_anim_set_duration(&a, kHoldMs * static_cast<uint32_t>(span) / 1000u);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_completed_cb(&a, completed);
    lv_anim_start(&a);
}

void HoldRing::reset() {
    lv_anim_delete(this, exec);
    done_ = nullptr;
    t_ = 0;
    if (obj_ != nullptr) lv_obj_invalidate(obj_);
}

void HoldRing::draw_cb(lv_event_t *e) {
    auto *self = static_cast<HoldRing *>(lv_event_get_user_data(e));
    const int32_t sweep = static_cast<int32_t>(
        std::lround(360.0f * ease_in_out_cubic(static_cast<float>(self->t_) / 1000.0f)));
    if (sweep <= 0) return;

    lv_area_t coords;
    lv_obj_get_coords(self->obj_, &coords);
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.center = {(coords.x1 + coords.x2 + 1) / 2, (coords.y1 + coords.y2 + 1) / 2};
    dsc.radius = static_cast<uint16_t>(lv_area_get_width(&coords) / 2);
    dsc.width = self->stroke_;
    lv_layer_t *layer = lv_event_get_layer(e);

    // LVGL arcs: 0 deg at three o'clock, clockwise. The ring starts at twelve (270).
    if (!self->gradient_) {
        dsc.color = self->left_;
        dsc.rounded = 1;
        dsc.start_angle = 270;
        dsc.end_angle = 270 + sweep;
        lv_draw_arc(layer, &dsc);
        return;
    }
    dsc.rounded = 0;
    for (int32_t from = 0; from < sweep; from += kSegmentDeg) {
        const int32_t to = from + kSegmentDeg + 1 < sweep ? from + kSegmentDeg + 1 : sweep;
        dsc.color = gradient_at(self->left_, self->right_, static_cast<float>(from + to) / 2.0f);
        dsc.start_angle = 270 + from;
        dsc.end_angle = 270 + to;
        lv_draw_arc(layer, &dsc);
    }
    // Round caps, as the solid ring has: a zero-length rounded arc at each end.
    dsc.rounded = 1;
    for (const int32_t at : {int32_t{0}, sweep}) {
        dsc.color = gradient_at(self->left_, self->right_, static_cast<float>(at));
        dsc.start_angle = 270 + at;
        dsc.end_angle = 270 + at + 1;
        lv_draw_arc(layer, &dsc);
    }
}
