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

// Supersampling per mask pixel: 4x4 gives 17 coverage levels, finer than LVGL's own anti-aliasing.
constexpr int kMaskSupersample = 4;

// Marker in mask space: axis vertical, base on the line y = `base_y`, point below it at `tip_y`,
// centred on x = `mid_x`. The point is filleted by a circle of radius `r` inscribed against both
// edges (half apex angle `alpha`), so inside the tangent points the test is the circle, outside it
// is the plain triangle.
struct MarkerShape {
    float mid_x, base_y, tip_y, base_half_w, r, sin_a;

    bool contains(float x, float y) const {
        if (y < base_y || y > tip_y) return false;
        const float depth = tip_y - base_y;
        if (std::fabs(x - mid_x) > base_half_w * (tip_y - y) / depth) return false;
        const float d = r / sin_a;                   // point -> fillet centre, along the axis
        const float tangent = d * (1.0f - sin_a * sin_a);  // point -> tangent points, along the axis
        if (tip_y - y >= tangent) return true;
        const float dx = x - mid_x, dy = y - (tip_y - d);
        return dx * dx + dy * dy <= r * r;
    }
};

void rasterize_a8(const MarkerShape &shape, int w, int h, uint8_t *out) {
    constexpr int kSamples = kMaskSupersample * kMaskSupersample;
    const float step = 1.0f / kMaskSupersample;
    for (int py = 0; py < h; ++py) {
        for (int px = 0; px < w; ++px) {
            int hits = 0;
            for (int sy = 0; sy < kMaskSupersample; ++sy) {
                const float y = static_cast<float>(py) + (sy + 0.5f) * step;
                for (int sx = 0; sx < kMaskSupersample; ++sx) {
                    const float x = static_cast<float>(px) + (sx + 0.5f) * step;
                    if (shape.contains(x, y)) ++hits;
                }
            }
            out[py * w + px] = static_cast<uint8_t>((hits * 255) / kSamples);
        }
    }
}

void init_a8(lv_image_dsc_t &img, const uint8_t *data, int w, int h) {
    img.header.magic = LV_IMAGE_HEADER_MAGIC;
    img.header.cf = LV_COLOR_FORMAT_A8;
    img.header.w = static_cast<uint32_t>(w);
    img.header.h = static_cast<uint32_t>(h);
    img.header.stride = static_cast<uint32_t>(w);
    img.data = data;
    img.data_size = static_cast<uint32_t>(w * h);
}

} // namespace

CompassRing::CompassRing(int32_t display_diameter_px)
    : display_diameter_px_(display_diameter_px) {
    build_marker_masks();
}

// Bakes the marker at scale 1, pointing down from the top of the ring (north-up). Per frame,
// draw() only rotates it about the base midpoint and zooms it with the ring's entry scale.
//
// The outline is the red marker grown outward by kNorthOutline on every side: each edge pushed out
// along its normal (the point moves in by o / sin(alpha)), the base pushed out past the rim, and
// the fillet radius grown by o about the SAME centre -- the offset of a rounded corner is a rounded
// corner, so a plain larger triangle with the red's own radius would read thinner at the tip.
void CompassRing::build_marker_masks() {
    const float half = static_cast<float>(display_diameter_px_) / 2.0f;
    const float depth = kNorthLength * half;
    const float base_half_w = kNorthWidth * half;
    const float alpha = std::atan2(base_half_w, depth);  // half apex angle
    const float sin_a = std::sin(alpha);
    const float r = kNorthTipRadius * half;
    const float o = kNorthOutline * half;
    const float outline_half_w = base_half_w + o / std::cos(alpha) + o * std::tan(alpha);

    // One transparent pixel of margin all round, so the rotation's bilinear sampling has empty
    // pixels to fade into. Integer pivot on an even width keeps the axis exactly on it.
    constexpr int kMargin = 1;
    const int rim_y = kMargin + static_cast<int>(std::ceil(o));
    int w = 2 * (static_cast<int>(std::ceil(outline_half_w)) + kMargin);
    int h = rim_y + static_cast<int>(std::ceil(depth + o / sin_a)) + kMargin;
    LV_ASSERT_MSG(w <= kMarkerMaskMaxSide && h <= kMarkerMaskMaxSide,
                  "compass marker mask too large for this display; raise kMarkerMaskMaxSide");
    if (w > kMarkerMaskMaxSide) w = kMarkerMaskMaxSide;
    if (h > kMarkerMaskMaxSide) h = kMarkerMaskMaxSide;

    const float mid_x = static_cast<float>(w) / 2.0f;
    const float rim = static_cast<float>(rim_y);
    const MarkerShape fill{mid_x, rim, rim + depth, base_half_w, r, sin_a};
    const MarkerShape outline{mid_x, rim - o, rim + depth + o / sin_a, outline_half_w, r + o, sin_a};
    rasterize_a8(outline, w, h, outline_mask_);
    rasterize_a8(fill, w, h, fill_mask_);
    init_a8(outline_img_, outline_mask_, w, h);
    init_a8(fill_img_, fill_mask_, w, h);
    marker_pivot_ = {w / 2, rim_y};
}

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
    const float rim_x = cx + std::cos(screen_angle) * half;
    const float rim_y = cy + std::sin(screen_angle) * half;

    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.opa = LV_OPA_COVER;
    dsc.recolor_opa = LV_OPA_COVER;
    dsc.antialias = true;
    dsc.pivot = marker_pivot_;
    // The mask is baked pointing down from the top (screen_angle -pi/2). lv_draw_image takes 0.1
    // degree units, positive clockwise -- the same sense as screen_angle, since y points down.
    int32_t rot_tenths =
        static_cast<int32_t>(std::lround((screen_angle + kPi / 2.0f) * 1800.0f / kPi)) % 3600;
    if (rot_tenths < 0) rot_tenths += 3600;
    dsc.rotation = rot_tenths;
    dsc.scale_x = dsc.scale_y = static_cast<int32_t>(std::lround(scale * LV_SCALE_NONE));

    // Placing the image so its pivot lands on the rim point keeps it fixed under rotation and zoom.
    lv_area_t area;
    area.x1 = static_cast<int32_t>(std::lround(rim_x)) - marker_pivot_.x;
    area.y1 = static_cast<int32_t>(std::lround(rim_y)) - marker_pivot_.y;
    area.x2 = area.x1 + static_cast<int32_t>(fill_img_.header.w) - 1;
    area.y2 = area.y1 + static_cast<int32_t>(fill_img_.header.h) - 1;

    // Outline first, then the red marker over it.
    dsc.src = &outline_img_;
    dsc.recolor = outline_color();
    lv_draw_image(layer, &dsc, &area);
    dsc.src = &fill_img_;
    dsc.recolor = north_color();
    lv_draw_image(layer, &dsc, &area);
}
