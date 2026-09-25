#include "compass_ring.hpp"

#include <cmath>

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Originally moved verbatim out of nav_renderer.cpp; the tick and marker figures below were then
// revised against "design reference/3.Turn by Turn/OpenApex Hardware Design Ref.svg".
// kDesignDiameter is nav_renderer's 600-unit authoring circle (maneuvers.json / nav_icons_data.h);
// the tick widths are authored against it, which is why the px conversion needs it.
constexpr float kDesignDiameter = 600.0f;

// 72 ticks, up from 36: a finer ring reads as a smoother instrument at a glance. kMajorEvery is 6
// rather than 3 so the majors still land every 30deg -- on the same twelve compass points they
// always marked. Leaving it at 3 would have doubled the majors too and lost that meaning.
constexpr int kTickCount = 72;
constexpr int kMajorEvery = 6;
constexpr float kMinorLength = 0.06f;
constexpr float kMajorLength = 0.10f;
// Majors are the same WIDTH as minors now, and are distinguished by colour (full white against the
// minors' grey) plus length, rather than by being twice as thick.
constexpr float kTickWidth = 3.0f;

// Marker geometry from the design ref: 16 wide at the rim, 16 deep (M128 0H112 L120 16), against a
// 120px half-frame.
constexpr float kNorthLength = 16.0f / 120.0f;
constexpr float kNorthWidth = 8.0f / 120.0f;
// Rounding on the marker's point, as a fraction of the half-frame (~1.4px at 240).
constexpr float kNorthTipRadius = 0.012f;

lv_color_t tick_color() { return lv_color_hex(0x585f68); }
lv_color_t major_tick_color() { return lv_color_white(); }
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

void CompassRing::draw(lv_layer_t *layer, const lv_area_t &coords, float scale) const {
    const float centre = static_cast<float>(display_diameter_px_) / 2.0f;
    const float cx = static_cast<float>(coords.x1) + centre;
    const float cy = static_cast<float>(coords.y1) + centre;
    // Everything below is sized off `half` and `px_unit`, so scaling them grows the whole ring
    // about the page centre.
    const float half = centre * scale;
    const float px_unit = static_cast<float>(display_diameter_px_) / kDesignDiameter * scale;

    for (int i = 0; i < kTickCount; ++i) {
        const float angle = (static_cast<float>(i) / kTickCount) * 2.0f * kPi - kPi / 2.0f;
        const bool is_major = (i % kMajorEvery) == 0;
        const float len = (is_major ? kMajorLength : kMinorLength) * half;
        const float w = std::max(1.0f, kTickWidth * px_unit);
        const float c = std::cos(angle), s = std::sin(angle);
        stroke_line(layer, cx + c * half, cy + s * half, cx + c * (half - len),
                    cy + s * (half - len), w, is_major ? major_tick_color() : tick_color());
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

    // Base corners, on the rim.
    const float ax = cx + nc * half + perp_x * base_half_w;
    const float ay = cy + ns * half + perp_y * base_half_w;
    const float bx = cx + nc * half - perp_x * base_half_w;
    const float by = cy + ns * half - perp_y * base_half_w;
    // The point, before rounding.
    const float tx = cx + nc * tip_r, ty = cy + ns * tip_r;

    // Rounded point. A filled triangle has no corner radius in LVGL, so the point is truncated and
    // the corner filled with a circle inscribed against both edges: centre `d` back from the point
    // along the axis, where d = r / sin(half apex angle), touching each edge at `d * cos(alpha)`
    // measured back along it. Union of the truncated body and that circle IS the rounded point --
    // stamping a circle on the full triangle would leave the sharp point poking through it.
    const float depth = half * kNorthLength;
    const float alpha = std::atan2(base_half_w, depth);
    const float sin_a = std::sin(alpha);
    const float r = kNorthTipRadius * half;

    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    dsc.color = north_color();
    dsc.opa = LV_OPA_COVER;

    if (r > 0.5f && sin_a > 0.01f) {
        const float d = r / sin_a;
        const float back = d * std::cos(alpha);
        // Unit vectors from the point back along each edge.
        float eax = ax - tx, eay = ay - ty;
        float ebx = bx - tx, eby = by - ty;
        const float la = std::sqrt(eax * eax + eay * eay);
        const float lb = std::sqrt(ebx * ebx + eby * eby);
        eax /= la; eay /= la; ebx /= lb; eby /= lb;
        const float pax = tx + eax * back, pay = ty + eay * back;
        const float pbx = tx + ebx * back, pby = ty + eby * back;

        // Truncated body as two triangles (LVGL draws triangles, not polygons).
        dsc.p[0] = to_lv(ax, ay);
        dsc.p[1] = to_lv(bx, by);
        dsc.p[2] = to_lv(pax, pay);
        lv_draw_triangle(layer, &dsc);
        dsc.p[0] = to_lv(bx, by);
        dsc.p[1] = to_lv(pbx, pby);
        dsc.p[2] = to_lv(pax, pay);
        lv_draw_triangle(layer, &dsc);

        // +nc, not -nc: the marker's base sits on the rim and its point aims INWARD, so moving
        // back from the point toward the base is the outward (increasing-radius) direction.
        const float ccx = tx + nc * d, ccy = ty + ns * d;
        lv_draw_rect_dsc_t cap;
        lv_draw_rect_dsc_init(&cap);
        cap.bg_color = north_color();
        cap.bg_opa = LV_OPA_COVER;
        cap.radius = LV_RADIUS_CIRCLE;
        lv_area_t a;
        a.x1 = static_cast<int32_t>(std::lround(ccx - r));
        a.y1 = static_cast<int32_t>(std::lround(ccy - r));
        a.x2 = static_cast<int32_t>(std::lround(ccx + r));
        a.y2 = static_cast<int32_t>(std::lround(ccy + r));
        lv_draw_rect(layer, &cap, &a);
        return;
    }

    dsc.p[0] = to_lv(ax, ay);
    dsc.p[1] = to_lv(bx, by);
    dsc.p[2] = to_lv(tx, ty);
    lv_draw_triangle(layer, &dsc);
}
