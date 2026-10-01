#include "odometer_screen.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "ease.hpp"
#include "gui_app.hpp"
#include "gui_font.hpp"
#include "units.hpp"

namespace {

// Geometry lifted from "design/screens/04-odometer/Odometer page ref.svg", in that file's
// 240px reference frame and scaled at runtime -- same convention as the idle screen.
//
// DIGIT COUNT: the SVG draws FOUR boxes (three full-size, then a smaller tenths box beside "km"),
// while the prose in design/screens/04-odometer/NOTES.md says five. The SVG is the Figma reference and wins, as it did
// for page 2's km-vs-miles. Flagged in the page doc; changing it is kDigitCount plus kBoxX.
constexpr float kBoxY = 100.0f;
constexpr float kBoxSize = 40.0f;
constexpr float kBoxRadius = 4.0f;
constexpr float kBoxStroke = 2.0f;
constexpr float kBoxX[3] = {39.0f, 86.0f, 133.0f};

// The tenths box: smaller, and top-aligned with the others rather than centred on them.
constexpr float kTenthsX = 179.75f;
constexpr float kTenthsY = 99.75f;
constexpr float kTenthsSize = 22.5f;
constexpr float kTenthsRadius = 4.25f;
constexpr float kTenthsStroke = 1.5f;

// Glyph heights measured off the SVG's outlined text, turned into a font size by Montserrat's
// ~0.70 cap-height-per-em ratio (the same derivation the idle screen's clock used).
constexpr float kFontPerGlyphH = 1.0f / 0.70f;
constexpr float kDigitGlyphH = 26.90f;    // -> 38px
constexpr float kTenthsGlyphH = 14.94f;   // -> 20px
constexpr float kHeadingGlyphH = 19.25f;  // "330 deg"  -> 28px
constexpr float kCardinalGlyphH = 14.25f; // "NW"       -> 20px
constexpr float kClockGlyphH = 19.50f;    // "12:55 am" -> 28px

// "km" is the one run with no capitals, so the cap-height ratio above does not apply to it: its
// 13.09px in the SVG is a lowercase ASCENDER, which is a larger fraction of the em. Measured
// against the reference, 18px is the face that matches; deriving it like the others rounded down
// to 14 and rendered the unit visibly undersized.
constexpr int32_t kKmFontPx = 18;

// Vertical centres of the text runs (midpoints of the SVG's rendered ink bands).
constexpr float kHeadingCy = 49.38f;  // band 39.75..59.00
constexpr float kCardinalCy = 71.63f; // band 64.50..78.75
constexpr float kClockCy = 173.50f;   // band 163.75..183.25
constexpr float kKmCy = 133.45f;
constexpr float kKmCx = 191.0f;

// Page-position dots. The FILLED one is the current page.
constexpr float kDot1Cx = 113.5f;
constexpr float kDot2Cx = 126.5f;
constexpr float kDotCy = 195.5f;
constexpr float kDot1R = 3.5f;
constexpr float kDot2R = 4.5f;
constexpr float kDotStroke = 2.0f;

constexpr float kPi = 3.14159265358979323846f;

// Entry, matching the nav page's timings (dial_screen.cpp): text first, the ring overlapping its end.
constexpr uint32_t kTextInMs = 600;
constexpr uint32_t kRingInDelayMs = 300;
constexpr uint32_t kRingInMs = 700;
constexpr float kRingEntryScale = 1.6f;

// #969696 in the SVG: the boxes and dots are deliberately dimmer than the white digits inside them,
// so the numbers read first at a glance.
lv_color_t chrome_color() { return lv_color_hex(0x969696); }

// UTF-8 degree sign, written as an escape so this file stays ASCII.
const char *const kDegree = "\xC2\xB0";

// 16-point compass, matching the "NW" in the reference. 0 deg = north, increasing clockwise.
const char *cardinal_for(uint16_t deg) {
    static const char *const kNames[16] = {"N",  "NNE", "NE", "ENE", "E",  "ESE", "SE", "SSE",
                                           "S",  "SSW", "SW", "WSW", "W",  "WNW", "NW", "NNW"};
    const int idx = static_cast<int>((static_cast<uint32_t>(deg % 360u) * 16u + 180u) / 360u) % 16;
    return kNames[idx];
}

} // namespace

int32_t OdometerScreen::px(float ref_240) const {
    return static_cast<int32_t>(std::lround(ref_240 * scale_));
}

// Fills `out` most-significant-first: three whole kilometres then one tenth. Wraps at 1000km,
// which is all four boxes hold.
// TODO(backend): capacity is a data-model question, not a layout one -- the odometer model (page
// doc gap 1) has to settle lifetime range and per-device vs per-phone before this is right.
void OdometerScreen::odometer_digits(uint32_t meters, char *out) {
    const uint32_t tenths = (meters / 100u) % 10000u; // 0..999.9km, in tenths of a km
    out[0] = static_cast<char>('0' + (tenths / 1000u) % 10u);
    out[1] = static_cast<char>('0' + (tenths / 100u) % 10u);
    out[2] = static_cast<char>('0' + (tenths / 10u) % 10u);
    out[3] = static_cast<char>('0' + tenths % 10u);
}

OdometerScreen::OdometerScreen()
    : compass_(lv_display_get_horizontal_resolution(lv_display_get_default())) {
    width_ = lv_display_get_horizontal_resolution(lv_display_get_default());
    scale_ = static_cast<float>(width_) / 240.0f;

    lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(root_, false);

    // --- Compass ring. Same draw-into-the-layer approach DialScreen uses: 36 ticks as widgets
    // would be 36 lv_obj_t for something that is never hit-tested or individually styled.
    compass_obj_ = lv_obj_create(root_);
    lv_obj_remove_style_all(compass_obj_);
    lv_obj_set_size(compass_obj_, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(compass_obj_, 0, 0);
    lv_obj_set_scrollable(compass_obj_, false);
    lv_obj_add_event_cb(compass_obj_, draw_event_cb, LV_EVENT_DRAW_MAIN, this);

    // --- Heading readout, in the theme colour per the page doc.
    heading_label_ = lv_label_create(root_);
    lv_obj_add_style(heading_label_, gui_accent_style(), 0);
    lv_obj_set_style_text_font(heading_label_,
                               montserrat_at_most(px(kHeadingGlyphH * kFontPerGlyphH)), 0);
    lv_label_set_text_fmt(heading_label_, "--%s", kDegree);

    cardinal_label_ = lv_label_create(root_);
    lv_obj_add_style(cardinal_label_, gui_accent_style(), 0);
    lv_obj_set_style_text_font(cardinal_label_,
                               montserrat_at_most(px(kCardinalGlyphH * kFontPerGlyphH)), 0);
    lv_label_set_text(cardinal_label_, "");

    // --- Odometer boxes. Borders only, no fill, so the black screen shows through.
    for (int i = 0; i < kDigitCount; ++i) {
        const bool tenths = (i == kDigitCount - 1);
        const float size = tenths ? kTenthsSize : kBoxSize;

        digit_box_[i] = lv_obj_create(root_);
        lv_obj_remove_style_all(digit_box_[i]);
        lv_obj_set_size(digit_box_[i], px(size), px(size));
        lv_obj_set_pos(digit_box_[i], px(tenths ? kTenthsX : kBoxX[i]),
                       px(tenths ? kTenthsY : kBoxY));
        lv_obj_set_style_border_color(digit_box_[i], chrome_color(), 0);
        lv_obj_set_style_border_width(
            digit_box_[i], std::max<int32_t>(1, px(tenths ? kTenthsStroke : kBoxStroke)), 0);
        lv_obj_set_style_border_opa(digit_box_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(digit_box_[i], px(tenths ? kTenthsRadius : kBoxRadius), 0);
        lv_obj_set_scrollable(digit_box_[i], false);

        digit_label_[i] = lv_label_create(digit_box_[i]);
        lv_obj_set_style_text_color(digit_label_[i], lv_color_white(), 0);
        lv_obj_set_style_text_font(
            digit_label_[i],
            montserrat_at_most(px((tenths ? kTenthsGlyphH : kDigitGlyphH) * kFontPerGlyphH)), 0);
        lv_label_set_text(digit_label_[i], "0");
        lv_obj_center(digit_label_[i]);
    }

    km_label_ = lv_label_create(root_);
    lv_obj_set_style_text_color(km_label_, lv_color_white(), 0);
    lv_obj_set_style_text_font(km_label_, montserrat_at_most(px(static_cast<float>(kKmFontPx))), 0);
    lv_label_set_text(km_label_, "km");

    clock_label_ = lv_label_create(root_);
    lv_obj_set_style_text_color(clock_label_, lv_color_white(), 0);
    lv_obj_set_style_text_font(clock_label_, montserrat_at_most(px(kClockGlyphH * kFontPerGlyphH)),
                               0);
    lv_label_set_text(clock_label_, "--:--");

    // --- Page dots: one outline, one filled, matching the SVG.
    for (int i = 0; i < 2; ++i) {
        const float r = (i == 0) ? kDot1R : kDot2R;
        dot_[i] = lv_obj_create(root_);
        lv_obj_remove_style_all(dot_[i]);
        lv_obj_set_size(dot_[i], px(r * 2.0f), px(r * 2.0f));
        lv_obj_set_pos(dot_[i], px((i == 0 ? kDot1Cx : kDot2Cx) - r), px(kDotCy - r));
        lv_obj_set_style_radius(dot_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_scrollable(dot_[i], false);
        if (i == 0) {
            lv_obj_set_style_border_color(dot_[i], chrome_color(), 0);
            lv_obj_set_style_border_width(dot_[i], std::max<int32_t>(1, px(kDotStroke)), 0);
            lv_obj_set_style_border_opa(dot_[i], LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_color(dot_[i], chrome_color(), 0);
            lv_obj_set_style_bg_opa(dot_[i], LV_OPA_COVER, 0);
        }
    }

    lv_obj_update_layout(root_);
    place_text();
    // Centre-aligned with offsets rather than positioned from a measured width, so it stays centred
    // when the unit setting swaps "km" for "mi".
    lv_obj_align(km_label_, LV_ALIGN_CENTER, px(kKmCx) - width_ / 2, px(kKmCy) - width_ / 2);
}

// Centre-anchored text has to be positioned from its measured height, so this runs after a layout
// pass rather than at creation time, and again whenever the text changes width or height.
void OdometerScreen::place_text() {
    lv_obj_align(heading_label_, LV_ALIGN_TOP_MID, 0,
                 px(kHeadingCy) - lv_obj_get_height(heading_label_) / 2);
    lv_obj_align(cardinal_label_, LV_ALIGN_TOP_MID, 0,
                 px(kCardinalCy) - lv_obj_get_height(cardinal_label_) / 2);
    lv_obj_align(clock_label_, LV_ALIGN_TOP_MID, 0,
                 px(kClockCy) - lv_obj_get_height(clock_label_) / 2);
}

void OdometerScreen::draw_event_cb(lv_event_t *e) {
    auto *self = static_cast<OdometerScreen *>(lv_event_get_user_data(e));
    lv_layer_t *layer = lv_event_get_layer(e);
    auto *obj = static_cast<lv_obj_t *>(lv_event_get_target(e));
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    const float ring = kRingEntryScale + (1.0f - kRingEntryScale) * static_cast<float>(self->ring_in_) / 1000.0f;
    self->compass_.draw(layer, coords, ring);
}

// Translation, not position, as the nav page's labels: the resting layout is never touched.
void OdometerScreen::set_text_in(void *var, int32_t value) {
    auto *self = static_cast<OdometerScreen *>(var);
    self->text_in_ = value;
    const int32_t travel = lv_display_get_vertical_resolution(lv_display_get_default()) * (1000 - value) / 1000;
    for (lv_obj_t *obj : {self->heading_label_, self->cardinal_label_, self->km_label_, self->clock_label_,
                          self->dot_[0], self->dot_[1]}) {
        lv_obj_set_style_translate_y(obj, travel, 0);
    }
    for (lv_obj_t *box : self->digit_box_) lv_obj_set_style_translate_y(box, travel, 0);
}

void OdometerScreen::set_ring_in(void *var, int32_t value) {
    auto *self = static_cast<OdometerScreen *>(var);
    self->ring_in_ = value;
    lv_obj_invalidate(self->compass_obj_);
}

void OdometerScreen::run(lv_anim_exec_xcb_t exec, int32_t from, int32_t to, uint32_t delay_ms, uint32_t ms,
                         lv_anim_path_cb_t path, lv_anim_completed_cb_t completed) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, this);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_delay(&a, delay_ms);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, path);
    lv_anim_set_early_apply(&a, true);
    lv_anim_set_completed_cb(&a, completed);
    lv_anim_start(&a);
}

void OdometerScreen::enter() {
    lv_anim_delete(this, nullptr);
    landed_ = false;
    set_text_in(this, 0);
    set_ring_in(this, 0);
    run(set_text_in, 0, 1000, 0, kTextInMs, lv_anim_path_ease_in_out, nullptr);
    run(set_ring_in, 0, 1000, kRingInDelayMs, kRingInMs, anim_path<ease_out_expo>,
        [](lv_anim_t *a) { static_cast<OdometerScreen *>(a->var)->landed_ = true; });
}

// Backwards from wherever the entry got to: the ring zooms out past the edge, then the text drops.
void OdometerScreen::leave(LeaveDone done, void *ctx) {
    lv_anim_delete(this, nullptr);
    done_ = done;
    done_ctx_ = ctx;
    run(set_ring_in, ring_in_, 0, 0, kRingInMs * static_cast<uint32_t>(ring_in_) / 1000u, anim_path<ease_in_expo>,
        [](lv_anim_t *a) {
            auto *s = static_cast<OdometerScreen *>(a->var);
            s->run(set_text_in, s->text_in_, 0, 0, kTextInMs * static_cast<uint32_t>(s->text_in_) / 1000u,
                   lv_anim_path_ease_in_out, [](lv_anim_t *a2) {
                       auto *s2 = static_cast<OdometerScreen *>(a2->var);
                       s2->landed_ = true;
                       s2->done_(s2->done_ctx_);
                   });
        });
}

void OdometerScreen::update(const terminal_view_state_t &state) {
    // --- Heading. 0xFFFF is "unknown" (view_state.h) and must stay visibly unknown rather than
    // becoming a fabricated north.
    if (state.heading_deg != last_heading_deg_) {
        last_heading_deg_ = state.heading_deg;
        if (state.heading_deg == NAV_U16_UNKNOWN) {
            lv_label_set_text_fmt(heading_label_, "--%s", kDegree);
            lv_label_set_text(cardinal_label_, "");
            compass_.set_north(0.0f, false);
        } else {
            const uint16_t deg = static_cast<uint16_t>(state.heading_deg % 360u);
            lv_label_set_text_fmt(heading_label_, "%u%s", static_cast<unsigned>(deg), kDegree);
            lv_label_set_text(cardinal_label_, cardinal_for(deg));
            compass_.set_north(static_cast<float>(deg) * kPi / 180.0f, true);
        }
        place_text();
        lv_obj_invalidate(compass_obj_);
    }

    if (state.odometer_meters != last_odometer_meters_ || units_imperial() != last_imperial_) {
        last_odometer_meters_ = state.odometer_meters;
        if (units_imperial() != last_imperial_) {
            last_imperial_ = units_imperial();
            lv_label_set_text(km_label_, units_long_label());
        }
        char digits[kDigitCount];
        odometer_digits(units_odometer_milli(state.odometer_meters), digits);
        for (int i = 0; i < kDigitCount; ++i) {
            const char text[2] = {digits[i], '\0'};
            lv_label_set_text(digit_label_[i], text);
        }
    }

    if (std::strncmp(state.clock, last_clock_, sizeof(last_clock_)) != 0) {
        std::snprintf(last_clock_, sizeof(last_clock_), "%s", state.clock);
        lv_label_set_text(clock_label_, state.clock[0] != '\0' ? state.clock : "--:--");
        place_text();
    }
}
