#include "compass_ring.hpp"

#include <cmath>

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Moved verbatim from nav_renderer.cpp so the nav dial's ring is unchanged by the extraction.
// kDesignDiameter is that renderer's 600-unit authoring circle (maneuvers.json / nav_icons_data.h);
// the tick widths below are authored against it, which is why the px conversion needs it.
constexpr float kDesignDiameter = 600.0f;
constexpr int kTickCount = 36;
constexpr int kMajorEvery = 3;
constexpr float kMinorLength = 0.06f;
constexpr float kMajorLength = 0.10f;
constexpr float kMinorWidth = 3.0f;
constexpr float kMajorWidth = 6.0f;
constexpr float kNorthLength = 0.10f;
constexpr float kNorthWidth = 0.045f;

lv_color_t tick_color() { return lv_color_hex(0x585f68); }
lv_color_t north_color() { return lv_color_hex(0xff3b30); }

// Even widths only, and rounded rather than truncated -- nav_renderer.cpp's rationale: LVGL's arc
// and line primitives disagree by a pixel otherwise. Kept identical here so the extracted ring is
// pixel-for-pixel what NavRenderer used to draw.
int32_t stroke_width_px(float width) {
    const int32_t w = 2 * static_cast<int32_t>(std::lround(width / 2.0f));
    return w < 2 ? 2 : w;
}

void stroke_line(lv_layer_t *layer, float x1, float y1, float x2, float y2, float width,
                 lv_color_t color) {
    if (width <= 0.0f) return;
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.p1 = {static_cast<lv_value_precise_t>(std::lround(x1)),
              static_cast<lv_value_precise_t>(std::lround(y1))};
    dsc.p2 = {static_cast<lv_value_precise_t>(std::lround(x2)),
              static_cast<lv_value_precise_t>(std::lround(y2))};
    dsc.color = color;
    dsc.width = stroke_width_px(width);
    lv_draw_line(layer, &dsc);
}

// Truncates, where stroke_line() above rounds. That asymmetry is inherited deliberately: it is
// what nav_renderer.cpp's to_lv()/stroke_line() pair did, and matching it keeps the nav dial's
// compass pixel-for-pixel identical across this extraction (verified by screenshot diff).
lv_point_precise_t to_lv(float x, float y) {
    return {static_cast<lv_value_precise_t>(x), static_cast<lv_value_precise_t>(y)};
}

} // namespace

CompassRing::CompassRing(int32_t display_diameter_px)
    : display_diameter_px_(display_diameter_px) {}

void CompassRing::set_north(float heading_rad, bool known) {
    north_heading_rad_ = heading_rad;
    north_known_ = known;
}

void CompassRing::draw(lv_layer_t *layer, const lv_area_t &coords) const {
    const float half = static_cast<float>(display_diameter_px_) / 2.0f;
    const float cx = static_cast<float>(coords.x1) + half;
    const float cy = static_cast<float>(coords.y1) + half;
    const float px_unit = static_cast<float>(display_diameter_px_) / kDesignDiameter;

    for (int i = 0; i < kTickCount; ++i) {
        const float angle = (static_cast<float>(i) / kTickCount) * 2.0f * kPi - kPi / 2.0f;
        const bool is_major = (i % kMajorEvery) == 0;
        const float len = (is_major ? kMajorLength : kMinorLength) * half;
        const float w = std::max(1.0f, (is_major ? kMajorWidth : kMinorWidth) * px_unit);
        const float c = std::cos(angle), s = std::sin(angle);
        stroke_line(layer, cx + c * half, cy + s * half, cx + c * (half - len),
                    cy + s * (half - len), w, tick_color());
    }

    if (!north_known_) return;
    // The nav page draws the route heading-up: the bike's forward direction is pinned to the top of
    // the screen, so the world -- north included -- rotates the OPPOSITE way to the heading. Hence
    // the negation. Without it the marker sweeps at exactly the right rate in exactly the wrong
    // direction, which is what "the compass spins backwards" on the 2026-09-23 ride was.
    // (-kPi/2 then converts "clockwise from up" to the atan2 convention, 0 = +x.)
    const float screen_angle = -north_heading_rad_ - kPi / 2.0f;
    const float nc = std::cos(screen_angle), ns = std::sin(screen_angle);
    const float tip_r = half * (1.0f - kNorthLength);
    const float base_half_w = kNorthWidth * half;
    const float perp_x = -ns, perp_y = nc;

    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    dsc.p[0] = to_lv(cx + nc * half + perp_x * base_half_w, cy + ns * half + perp_y * base_half_w);
    dsc.p[1] = to_lv(cx + nc * half - perp_x * base_half_w, cy + ns * half - perp_y * base_half_w);
    dsc.p[2] = to_lv(cx + nc * tip_r, cy + ns * tip_r);
    dsc.color = north_color();
    dsc.opa = LV_OPA_COVER;
    lv_draw_triangle(layer, &dsc);
}
