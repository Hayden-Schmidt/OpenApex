#include "dial_screen.hpp"

#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace {

constexpr float kPi = 3.14159265358979323846f;

lv_point_precise_t pt(float x, float y) {
    return {static_cast<lv_value_precise_t>(x), static_cast<lv_value_precise_t>(y)};
}

lv_point_precise_t point_at(float cx, float cy, float angle_deg, float radius) {
    const float rad = angle_deg * kPi / 180.0f;
    return pt(cx + std::sin(rad) * radius, cy - std::cos(rad) * radius);
}

// nav_icon_t (firmware/main/nav_model.h) is the stable cross-platform protocol enum and is not
// touched here -- this maps each value onto the closest maneuver NavRenderer actually has data
// for (Google Icons Archive/Icon JSON/maneuvers.json). ARRIVED/UNKNOWN have no equivalent and are
// handled separately in DialScreen::update()/draw_status_glyph().
bool render_icon_for(nav_icon_t icon, nav_render_icon_t *out) {
    switch (icon) {
        case NAV_ICON_STRAIGHT:
            *out = NAV_RENDER_STRAIGHT;
            return true;
        case NAV_ICON_TURN_LEFT:
            *out = NAV_RENDER_TURN_LEFT;
            return true;
        case NAV_ICON_TURN_RIGHT:
            *out = NAV_RENDER_TURN_RIGHT;
            return true;
        // Sharp turns now have their own geometry (125 deg bend, 45 radius, in maneuvers.json)
        // rather than borrowing the 90 deg turn. Reusing the ordinary turn drew a sharp turn and
        // a normal one identically, so the display could not tell the rider which one was coming.
        case NAV_ICON_SHARP_LEFT:
            *out = NAV_RENDER_TURN_SHARP_LEFT;
            return true;
        case NAV_ICON_SHARP_RIGHT:
            *out = NAV_RENDER_TURN_SHARP_RIGHT;
            return true;
        case NAV_ICON_SLIGHT_LEFT:
            *out = NAV_RENDER_TURN_SLIGHT_LEFT;
            return true;
        case NAV_ICON_SLIGHT_RIGHT:
            *out = NAV_RENDER_TURN_SLIGHT_RIGHT;
            return true;
        case NAV_ICON_ROUNDABOUT_LEFT:
            *out = NAV_RENDER_ROUNDABOUT_LEFT;
            return true;
        case NAV_ICON_ROUNDABOUT_RIGHT:
            *out = NAV_RENDER_ROUNDABOUT_RIGHT;
            return true;
        case NAV_ICON_ROUNDABOUT_STRAIGHT:
            *out = NAV_RENDER_ROUNDABOUT_STRAIGHT;
            return true;
        case NAV_ICON_U_TURN:
            *out = NAV_RENDER_U_TURN_RIGHT;
            return true;
        default: // ARRIVED, UNKNOWN
            return false;
    }
}

} // namespace

// Uses the actual runtime display resolution (not board_profile.h's BOARD_DISP_WIDTH macro) so
// this code works unmodified whatever resolution the active lv_display was created at -- on real
// hardware that's always BOARD_DISP_WIDTH anyway, but it lets firmware/sim_lvgl pick a resolution
// at runtime without recompiling firmware/gui.
DialScreen::DialScreen()
    : renderer_(lv_display_get_horizontal_resolution(lv_display_get_default())),
      status_box_size_((lv_display_get_horizontal_resolution(lv_display_get_default()) * 55) / 100) {
    lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);

    // NavRenderer strokes straight into this object's LV_EVENT_DRAW_MAIN layer. An lv_canvas was
    // tried here instead, on the theory that compositing the arrow's overlapping strokes onto one
    // surface was needed to stop their anti-aliased joins reading as hairlines -- but the draw
    // layer is already a pixel buffer holding the background, so each stroke blends against the
    // previous one's solid pixels there just the same. With every stroke fully opaque the two are
    // pixel-identical, and the canvas would have cost a display-sized ARGB8888 buffer (225kB at
    // 240x240) the C3 does not have.
    canvas_obj_ = lv_obj_create(root_);
    lv_obj_set_size(canvas_obj_, lv_pct(100), lv_pct(100));
    lv_obj_center(canvas_obj_);
    lv_obj_set_style_bg_opa(canvas_obj_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(canvas_obj_, 0, 0);
    lv_obj_set_style_pad_all(canvas_obj_, 0, 0);
    lv_obj_set_scrollable(canvas_obj_, false);
    lv_obj_add_event_cb(canvas_obj_, draw_event_cb, LV_EVENT_DRAW_MAIN, this);

    status_shaft_ = lv_line_create(root_);
    status_head_ = lv_line_create(root_);
    for (lv_obj_t *line : {status_shaft_, status_head_}) {
        lv_obj_set_size(line, status_box_size_, status_box_size_);
        lv_obj_set_style_line_width(line, status_box_size_ / 16, 0);
        lv_obj_set_style_line_rounded(line, true, 0);
        lv_obj_set_style_line_color(line, lv_color_white(), 0);
        lv_obj_align(line, LV_ALIGN_CENTER, 0, -(status_box_size_ / 8));
        lv_obj_set_hidden(line, true);
    }

    distance_label_ = lv_label_create(root_);
    lv_obj_set_style_text_color(distance_label_, lv_color_white(), 0);
    lv_obj_align(distance_label_, LV_ALIGN_BOTTOM_MID, 0, -20);
}

void DialScreen::draw_event_cb(lv_event_t *e) {
    auto *self = static_cast<DialScreen *>(lv_event_get_user_data(e));
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *obj = static_cast<lv_obj_t *>(lv_event_get_target(e));
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    self->renderer_.draw(layer, coords);
}

void DialScreen::draw_status_glyph(nav_icon_t icon) {
    const float S = static_cast<float>(status_box_size_);
    const float cx = S * 0.5f;
    const float cy = S * 0.5f;
    uint32_t shaft_count = 0;
    uint32_t head_count = 0;

    if (icon == NAV_ICON_ARRIVED) {
        // Full ring (completion badge) + a checkmark drawn into the head-line buffer.
        const float kRadius = S * 0.28f;
        for (float t = 0.0f; t <= 360.0f + 0.1f && shaft_count < kStatusShaftMaxPts;
             t += kStatusRingStepDeg, ++shaft_count) {
            status_shaft_pts_[shaft_count] = point_at(cx, cy, t, kRadius);
        }
        status_head_pts_[0] = pt(cx - S * 0.12f, cy + S * 0.02f);
        status_head_pts_[1] = pt(cx + S * 0.14f, cy - S * 0.10f);
        head_count = 2;
    } else { // NAV_ICON_UNKNOWN
        status_shaft_pts_[0] = pt(cx - S * 0.32f, cy + S * 0.28f);
        status_shaft_pts_[1] = pt(cx, cy - S * 0.32f);
        status_shaft_pts_[2] = pt(cx + S * 0.32f, cy + S * 0.28f);
        status_shaft_pts_[3] = status_shaft_pts_[0];
        shaft_count = 4;
        status_head_pts_[0] = pt(cx, cy - S * 0.06f);
        status_head_pts_[1] = pt(cx, cy + S * 0.12f);
        head_count = 2;
    }
    lv_line_set_points_mutable(status_shaft_, status_shaft_pts_, shaft_count);
    lv_line_set_points_mutable(status_head_, status_head_pts_, head_count);
}

void DialScreen::update(const terminal_view_state_t &state) {
    // distance_meters is meaningful in VIEW_ACTIVE and VIEW_STALE (view_state.h) -- blank it
    // otherwise. VIEW_STALE is last-known-good, not garbage: it renders greyed rather than hidden,
    // because a held distance reads far better on the road than an empty dial.
    if (state.state == VIEW_ACTIVE || state.state == VIEW_STALE) {
        char text[16];
        std::snprintf(text, sizeof(text), "%u m", static_cast<unsigned>(state.distance_meters));
        lv_label_set_text(distance_label_, text);
        lv_obj_set_style_text_color(distance_label_,
                                    state.state == VIEW_STALE ? lv_color_hex(0x808080)
                                                              : lv_color_white(),
                                    0);
    } else {
        lv_label_set_text(distance_label_, "");
    }

    nav_render_icon_t render_icon;
    const bool has_render_icon = render_icon_for(state.icon_type, &render_icon);

    if (state.icon_type != last_icon_) {
        if (has_render_icon) {
            lv_obj_set_hidden(status_shaft_, true);
            lv_obj_set_hidden(status_head_, true);
            renderer_.add_maneuver(render_icon, /*animate=*/last_icon_ != static_cast<nav_icon_t>(-1));
        } else {
            draw_status_glyph(state.icon_type);
            lv_obj_set_hidden(status_shaft_, false);
            lv_obj_set_hidden(status_head_, false);
        }
        last_icon_ = state.icon_type;
        lv_obj_invalidate(canvas_obj_);
    }

    // Heading-up ring: 0xFFFF = unknown (view_state.h). Smoothing (circular low-pass + 90 deg/s
    // slew cap, at the 10Hz countdown_task tick) already happened upstream in heading_fusion.c;
    // this is just the display of an already-smoothed value, not a raw passthrough.
    const bool heading_known = state.heading_deg != 0xFFFFu;
    renderer_.set_north_heading(state.heading_deg * kPi / 180.0f, heading_known);
    const bool heading_changed = state.heading_deg != last_heading_deg_;
    last_heading_deg_ = state.heading_deg;

    const bool tween_active = has_render_icon && renderer_.tick(lv_tick_get());
    if (tween_active || (has_render_icon && heading_changed)) {
        lv_obj_invalidate(canvas_obj_);
    }
}
