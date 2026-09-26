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
constexpr float kNorthLength = 16.0f / 120.0f;  // design ref 16
constexpr float kNorthWidth = 8.0f / 120.0f;    // design ref 8
// Fillet on the marker's point, as a fraction of the half-frame (2.5px at 240): enough to take the
// needle-sharp edge off, not so much that the point reads as blunt.
constexpr float kNorthTipRadius = 2.5f / 120.0f;
// Triangles in the fillet's fan. Plenty at a few pixels' radius.
constexpr int kNorthTipSteps = 6;
// Dark border around the marker, as a fraction of the half-frame (4px at 240).
constexpr float kNorthOutline = 4.0f / 120.0f;

lv_color_t tick_color() { return lv_color_hex(0x585f68); }
lv_color_t major_tick_color() { return lv_color_white(); }
lv_color_t north_color() { return lv_color_hex(0xff3b30); }
lv_color_t outline_color() { return lv_color_hex(0x1a1a1a); }

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

// One filleted marker triangle, base on the circle of radius `base_r` about (cx, cy) and point at
// `tip_r`, aimed along `angle` (atan2 convention) -- inward, since tip_r < base_r.
//
// A filled triangle has no corner radius in LVGL, so the point is cut back to the two tangent
// points of a circle of radius `r` inscribed against both edges, and the arc between them is filled
// as a fan of triangles back to the base. The fan joins the edges exactly at the tangent points in
// sub-pixel coordinates -- a separate integer-snapped circle stamped on the cut end (an earlier
// approach) sat proud of the edges and read as a nub rather than a rounded tip. The circle's centre
// is `d` back from the point along the axis, where d = r / sin(half apex angle).
void draw_marker(lv_layer_t *layer, float cx, float cy, float angle, float tip_r, float base_r,
                 float base_half_w, float r, lv_color_t color) {
    const float nc = std::cos(angle), ns = std::sin(angle);
    const float perp_x = -ns, perp_y = nc;
    const float ax = cx + nc * base_r + perp_x * base_half_w;
    const float ay = cy + ns * base_r + perp_y * base_half_w;
    const float bx = cx + nc * base_r - perp_x * base_half_w;
    const float by = cy + ns * base_r - perp_y * base_half_w;
    const float tx = cx + nc * tip_r, ty = cy + ns * tip_r;

    const float alpha = std::atan2(base_half_w, base_r - tip_r);
    const float sin_a = std::sin(alpha);

    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    dsc.color = color;
    dsc.opa = LV_OPA_COVER;

    if (r <= 0.5f || sin_a <= 0.01f) {
        dsc.p[0] = to_lv(ax, ay);
        dsc.p[1] = to_lv(bx, by);
        dsc.p[2] = to_lv(tx, ty);
        lv_draw_triangle(layer, &dsc);
        return;
    }

    // +nc: the base sits outward of the point, so moving back from the point is +nc.
    const float d = r / sin_a;
    const float ccx = tx + nc * d, ccy = ty + ns * d;
    // Tangent points sit at +-(90deg - alpha) either side of the inward axis, seen from the
    // centre; the arc between them runs through the inward axis itself.
    const float inward = std::atan2(-ns, -nc);
    const float sweep = kPi / 2.0f - alpha;
    const float mx = (ax + bx) * 0.5f, my = (ay + by) * 0.5f;  // fan hub: base midpoint

    float px = ccx + r * std::cos(inward + sweep), py = ccy + r * std::sin(inward + sweep);
    const float qx = ccx + r * std::cos(inward - sweep), qy = ccy + r * std::sin(inward - sweep);
    // Body: base corners to the two tangent points. perp (+) is the `a` side.
    const bool p_is_a = (px - tx) * perp_x + (py - ty) * perp_y > 0.0f;
    dsc.p[0] = to_lv(ax, ay);
    dsc.p[1] = to_lv(bx, by);
    dsc.p[2] = p_is_a ? to_lv(qx, qy) : to_lv(px, py);
    lv_draw_triangle(layer, &dsc);
    dsc.p[0] = to_lv(ax, ay);
    dsc.p[1] = p_is_a ? to_lv(qx, qy) : to_lv(px, py);
    dsc.p[2] = p_is_a ? to_lv(px, py) : to_lv(qx, qy);
    lv_draw_triangle(layer, &dsc);

    for (int i = 1; i <= kNorthTipSteps; ++i) {
        const float a = inward + sweep - 2.0f * sweep * static_cast<float>(i) / kNorthTipSteps;
        const float nx = ccx + r * std::cos(a), ny = ccy + r * std::sin(a);
        dsc.p[0] = to_lv(mx, my);
        dsc.p[1] = to_lv(px, py);
        dsc.p[2] = to_lv(nx, ny);
        lv_draw_triangle(layer, &dsc);
        px = nx;
        py = ny;
    }
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
    const float tip_r = half * (1.0f - kNorthLength);
    const float base_half_w = kNorthWidth * half;
    const float alpha = std::atan2(base_half_w, half * kNorthLength);  // half apex angle
    const float r = kNorthTipRadius * half;

    // Outline first, then the red marker over it. The outline is the same marker grown outward by
    // kNorthOutline on every side: each edge pushed out along its normal (the point moves in by
    // o / sin(alpha)), the base pushed out past the rim, and the fillet radius grown by o about the
    // SAME centre -- the offset of a rounded corner is a rounded corner, so a plain larger triangle
    // with the red's own radius would read thinner at the tip than along the sides.
    const float o = kNorthOutline * half;
    draw_marker(layer, cx, cy, screen_angle, tip_r - o / std::sin(alpha), half + o,
                base_half_w + o / std::cos(alpha) + o * std::tan(alpha), r + o, outline_color());
    draw_marker(layer, cx, cy, screen_angle, tip_r, half, base_half_w, r, north_color());
}
