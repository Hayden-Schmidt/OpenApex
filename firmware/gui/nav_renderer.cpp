#include "nav_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "ease.hpp"

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDesignDiameter = 600.0f; // maneuvers.json / nav_icons_data.h authoring circle

// --- ported knobs (Demo/params.js) ----------------------------------------------------------
constexpr float kDisplayScale = 1.1f;
constexpr float kLineThickness = 22.0f;     // route units, before displayScale
constexpr float kArrowheadScale = 0.25f;
// Route units, screen space. Shifts the whole maneuver up so it clears the street/distance stack
// at the bottom of the page. -28.545 puts the arrow exactly where "OpenApex Hardware Design Ref.svg"
// draws it: one route unit is kDisplayScale * (diameter/600) px, i.e. 0.44px at 240, and the design
// moved the arrow 13px up from the old +1.0 offset.
constexpr float kVerticalOffset = -28.545f;
constexpr float kSeamOverlap = 0.5f;        // px
constexpr float kRevealSpeed = 0.9f;        // route units / ms
constexpr uint32_t kMinRevealMs = 1000;
constexpr uint32_t kMaxRevealMs = 1200;

// HEAD_DEPTH (Demo/demo.js): how far back from the arrowhead's tip its glyph is already at full
// width -- the rendered line is trimmed short by this much so it tucks under the glyph.
const float kHeadDepth = NAV_ARROWHEAD_HEAD_BASE_DEPTH * kArrowheadScale;

lv_color_t route_color() { return lv_color_white(); }

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// JS's normAngle(): normalize radians to (-PI, PI], used to take the short way round a rotation.
float norm_angle(float a) {
    a = std::fmod(a, 2.0f * kPi);
    if (a > kPi) a -= 2.0f * kPi;
    if (a <= -kPi) a += 2.0f * kPi;
    return a;
}

nav_pt_t rotate_point(const nav_pt_t &p, const nav_pt_t &origin, float angle) {
    const float dx = p.x - origin.x;
    const float dy = p.y - origin.y;
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return {origin.x + dx * c - dy * s, origin.y + dx * s + dy * c};
}

lv_point_precise_t to_lv(const nav_pt_t &p) {
    return {static_cast<lv_value_precise_t>(p.x), static_cast<lv_value_precise_t>(p.y)};
}

// -- LVGL-shaped drawing primitives (C++ twin of Demo/lvgl_shim.js) --------------------------

constexpr int kMaxPolyVerts = 8;

// Offsets a convex polygon (<= kMaxPolyVerts points) outward by `d` so adjacent filled pieces overlap
// instead of abut -- see lvgl_shim.js's SEAMS note for why this exists. Direct port of its
// inflate(); MITER_LIMIT matches.
constexpr float kMiterLimit = 2.5f;

int inflate(const nav_pt_t *pts, int n, float d, nav_pt_t *out) {
    if (d == 0.0f) {
        std::memcpy(out, pts, sizeof(nav_pt_t) * n);
        return n;
    }
    float cx = 0.0f, cy = 0.0f;
    for (int i = 0; i < n; ++i) {
        cx += pts[i].x;
        cy += pts[i].y;
    }
    cx /= n;
    cy /= n;

    struct Line {
        bool valid;
        float px, py, dx, dy;
    };
    Line lines[kMaxPolyVerts];
    for (int i = 0; i < n; ++i) {
        const nav_pt_t &p = pts[i];
        const nav_pt_t &q = pts[(i + 1) % n];
        float dx = q.x - p.x, dy = q.y - p.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-9f) {
            lines[i].valid = false;
            continue;
        }
        dx /= len;
        dy /= len;
        float nx = -dy, ny = dx;
        if (nx * (p.x - cx) + ny * (p.y - cy) < 0.0f) {
            nx = -nx;
            ny = -ny;
        }
        lines[i] = {true, p.x + nx * d, p.y + ny * d, dx, dy};
    }

    for (int i = 0; i < n; ++i) {
        const Line &a = lines[(i + n - 1) % n];
        const Line &b = lines[i];
        if (!a.valid || !b.valid) {
            out[i] = pts[i];
            continue;
        }
        const float cross = a.dx * b.dy - a.dy * b.dx;
        if (std::fabs(cross) < 1e-9f) {
            out[i] = pts[i];
            continue;
        }
        const float t = ((b.px - a.px) * b.dy - (b.py - a.py) * b.dx) / cross;
        const float mx = a.px + a.dx * t, my = a.py + a.dy * t;
        const float ox = mx - pts[i].x, oy = my - pts[i].y;
        const float run = std::sqrt(ox * ox + oy * oy);
        if (run > d * kMiterLimit) {
            const float k = (d * kMiterLimit) / run;
            out[i] = {pts[i].x + ox * k, pts[i].y + oy * k};
        } else {
            out[i] = {mx, my};
        }
    }
    return n;
}

void fill_triangle(lv_layer_t *layer, const nav_pt_t &a, const nav_pt_t &b, const nav_pt_t &c,
                    lv_color_t color, float grow = 0.0f) {
    const nav_pt_t tri[3] = {a, b, c};
    nav_pt_t grown[3];
    inflate(tri, 3, grow, grown);

    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    dsc.p[0] = to_lv(grown[0]);
    dsc.p[1] = to_lv(grown[1]);
    dsc.p[2] = to_lv(grown[2]);
    dsc.color = color;
    dsc.opa = LV_OPA_COVER;
    lv_draw_triangle(layer, &dsc);
}

// -- polygon -> 8-bit coverage mask ------------------------------------------------------------
// Runs once at construction (see NavRenderer::build_arrowhead_mask), never per frame, so a plain
// supersampled even-odd fill is fine -- no need for a scanline/active-edge rasterizer. 4x4 samples
// give 17 coverage levels, which is finer than the anti-aliasing LVGL's own primitives produce.
constexpr int kMaskSupersample = 4;

bool point_in_poly(const nav_pt_t *pts, int n, float x, float y) {
    bool inside = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        if ((pts[i].y > y) == (pts[j].y > y)) continue;
        const float t = (y - pts[i].y) / (pts[j].y - pts[i].y);
        if (x < pts[i].x + t * (pts[j].x - pts[i].x)) inside = !inside;
    }
    return inside;
}

void rasterize_poly_a8(const nav_pt_t *pts, int n, int w, int h, uint8_t *out) {
    constexpr int kSamples = kMaskSupersample * kMaskSupersample;
    const float step = 1.0f / kMaskSupersample;
    for (int py = 0; py < h; ++py) {
        for (int px = 0; px < w; ++px) {
            int hits = 0;
            for (int sy = 0; sy < kMaskSupersample; ++sy) {
                const float y = static_cast<float>(py) + (sy + 0.5f) * step;
                for (int sx = 0; sx < kMaskSupersample; ++sx) {
                    const float x = static_cast<float>(px) + (sx + 0.5f) * step;
                    if (point_in_poly(pts, n, x, y)) ++hits;
                }
            }
            out[py * w + px] = static_cast<uint8_t>((hits * 255) / kSamples);
        }
    }
}

// lv_draw_arc() swept the full circle with width == radius fills a solid disc.
void fill_circle(lv_layer_t *layer, float cx, float cy, float r, lv_color_t color) {
    if (r <= 0.0f) return;
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.color = color;
    dsc.opa = LV_OPA_COVER;
    dsc.center = {static_cast<int32_t>(std::lround(cx)), static_cast<int32_t>(std::lround(cy))};
    dsc.radius = static_cast<uint16_t>(std::lround(r));
    dsc.width = static_cast<int32_t>(std::lround(r));
    dsc.start_angle = 0;
    dsc.end_angle = 360;
    lv_draw_arc(layer, &dsc);
}

// The one stroke width every primitive uses, in whole pixels and always even.
//
// Whole, because LVGL takes an integer width either way; even, because an arc's band centreline
// sits at `radius - width/2`, and an odd width puts that centreline on a half pixel, which no
// choice of integer radius can then line up with a line's integer endpoint.
//
// Shared, because it used to not be: stroke_line() truncated the float width while stroke_arc()
// rounded it, so a 15.6px stroke drew straights 15px wide and curves 16px wide. A one-pixel width
// difference between a straight and the curve it runs into offsets one edge of the silhouette by
// a whole pixel, which reads as the curve sitting slightly to the side of the straight.
int32_t stroke_width_px(float width) {
    const int32_t w = 2 * static_cast<int32_t>(std::lround(width / 2.0f));
    return w < 2 ? 2 : w;
}

// `anchor`, when non-null, is the pixel the preceding segment actually finished on. The line is
// then translated BODILY onto it -- both endpoints by the same delta -- rather than having its
// start pulled across, which would tilt it. A stem is normally axis-aligned and therefore
// pixel-crisp; tilting it by even half a degree anti-aliases it along its whole length, so the
// error is spent on the line's position (invisible) instead of its angle (very visible).
// `out_end` reports where the far end was really drawn, for the next segment to anchor onto.
void stroke_line(lv_layer_t *layer, float x1, float y1, float x2, float y2, float width,
                  lv_color_t color, bool rounded = false, const nav_pt_t *anchor = nullptr,
                  nav_pt_t *out_end = nullptr) {
    if (width <= 0.0f) return;
    if (anchor != nullptr) {
        const float dx = anchor->x - std::round(x1), dy = anchor->y - std::round(y1);
        x1 += dx; y1 += dy; x2 += dx; y2 += dy;
    }
    if (out_end != nullptr) *out_end = {std::round(x2), std::round(y2)};
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    // ROUND, never truncate. With LV_USE_FLOAT off, lv_value_precise_t is an integer, so a plain
    // cast floors every endpoint while stroke_arc() rounds its centre -- a systematic half-pixel
    // disagreement between a line and the arc it is supposed to be tangent to, which showed up as
    // a visible lip at every straight-to-curve join.
    dsc.p1 = {static_cast<lv_value_precise_t>(std::lround(x1)),
              static_cast<lv_value_precise_t>(std::lround(y1))};
    dsc.p2 = {static_cast<lv_value_precise_t>(std::lround(x2)),
              static_cast<lv_value_precise_t>(std::lround(y2))};
    dsc.color = color;
    dsc.width = stroke_width_px(width);
    dsc.round_start = rounded;
    dsc.round_end = rounded;
    lv_draw_line(layer, &dsc);
}

// A curve is one native LVGL arc -- the reason this renderer carries primitives rather than
// sampled points.
//
// `anchor` is where this arc's start point was drawn by whatever it joins onto, in screen pixels.
// The centre is then placed RELATIVE TO THAT rather than rounded off the ideal float centre.
//
// The reason is that lv_draw_arc quantises an arc as a rigid body: rounding dsc.center shifts the
// whole curve by up to half a pixel, and rounding dsc.radius shifts it another half pixel along
// its own radius. Both of those were rounded from the ideal geometry with no reference to where
// the adjoining straight actually landed, which was rounded separately again -- so the curve
// could sit a full pixel to the side of the straight it is supposed to flow out of. Against a
// vertical stem the radius at the tangent point is horizontal, which is why it read as the arc
// being pushed sideways.
//
// Deriving the centre from the anchor ties the two together: the arc translates bodily to meet
// the straight, and a circle moved half a pixel is still a circle, whereas a straight moved half
// a pixel loses its axis alignment and anti-aliases along its whole length. When the tangent is
// axis-aligned -- the usual resting case, stem heading up -- `anchor + r_center * unit` is
// integer plus integer, so the join is exact rather than merely close.
void stroke_arc(lv_layer_t *layer, const nav_pt_t &center, const nav_pt_t &anchor, float radius,
                 float start_deg, float end_deg, float width, lv_color_t color,
                 bool path_end_at_start = false, nav_pt_t *out_end = nullptr) {
    if (radius <= 0.0f || width <= 0.0f || end_deg <= start_deg) return;
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.color = color;
    dsc.opa = LV_OPA_COVER;

    const int32_t width_i = stroke_width_px(width);
    const int32_t half_i = width_i / 2;
    int32_t r_center = static_cast<int32_t>(std::lround(radius));
    if (r_center < 1) r_center = 1;

    const float ux = (center.x - anchor.x) / radius;
    const float uy = (center.y - anchor.y) / radius;
    const float ax = std::round(anchor.x);
    const float ay = std::round(anchor.y);
    dsc.center = {static_cast<int32_t>(std::lround(ax + ux * static_cast<float>(r_center))),
                  static_cast<int32_t>(std::lround(ay + uy * static_cast<float>(r_center)))};
    // lv_draw_arc fills the band [radius - width, radius], so the outer radius is the integer
    // centreline pushed out by the (integral) half width -- the band centreline is exactly
    // r_center, with no rounding left over.
    dsc.radius = static_cast<uint16_t>(r_center + half_i);
    dsc.width = width_i;
    dsc.rounded = 1;
    // ROUND these too. start_angle/end_angle are lv_value_precise_t, i.e. whole degrees with
    // LV_USE_FLOAT off, so a plain cast truncates both ends inward and the rounded cap is then
    // centred on the displaced endpoint.
    dsc.start_angle = static_cast<lv_value_precise_t>(std::lround(start_deg));
    dsc.end_angle = static_cast<lv_value_precise_t>(std::lround(end_deg));
    lv_draw_arc(layer, &dsc);

    // Report the pixel this curve really finished on, computed from the values LVGL was handed --
    // integer centre, integer centreline radius, whole-degree angle. lv_draw_arc only ever sweeps
    // start -> end in increasing degrees, so for a left-bending curve the path's end is the START
    // of the sweep.
    if (out_end != nullptr) {
        const float a = static_cast<float>(path_end_at_start ? dsc.start_angle : dsc.end_angle) *
                        kPi / 180.0f;
        *out_end = {static_cast<float>(dsc.center.x) + static_cast<float>(r_center) * std::cos(a),
                    static_cast<float>(dsc.center.y) + static_cast<float>(r_center) * std::sin(a)};
    }
}

float deg_of(float rad) { return rad * 180.0f / kPi; }

} // namespace

NavRenderer::NavRenderer(int32_t display_diameter_px)
    : compass_(display_diameter_px), display_diameter_px_(display_diameter_px) {
    build_arrowhead_mask();
}

// Rasterizes NAV_ARROWHEAD_PERIMETER into head_mask_ at its exact final on-screen size, in the
// glyph's authored orientation (tip at origin, pointing "up"/-y). The size is the same product the
// polygon fill used to apply per vertex -- kArrowheadScale (the authored glyph size knob, kept in
// sync with arrowheadScale in Demo/params.js) x kDisplayScale x px_per_unit() -- so changing the
// authored scale still changes the rendered glyph, it just re-bakes the mask instead of re-scaling
// vertices. Rotation is NOT baked in: that is the one thing that varies per frame, and
// lv_draw_image applies it.
void NavRenderer::build_arrowhead_mask() {
    const float scale = kArrowheadScale * kDisplayScale * px_per_unit();

    float min_x = 0.0f, min_y = 0.0f, max_x = 0.0f, max_y = 0.0f;
    for (int i = 0; i < NAV_ARROWHEAD_VERT_COUNT; ++i) {
        const float x = NAV_ARROWHEAD_PERIMETER[i].x * scale;
        const float y = NAV_ARROWHEAD_PERIMETER[i].y * scale;
        min_x = i == 0 ? x : (x < min_x ? x : min_x);
        max_x = i == 0 ? x : (x > max_x ? x : max_x);
        min_y = i == 0 ? y : (y < min_y ? y : min_y);
        max_y = i == 0 ? y : (y > max_y ? y : max_y);
    }

    // One transparent pixel of margin all round, so the rotation's bilinear sampling has empty
    // pixels to fade into rather than clamping the glyph's own edge and hardening it.
    constexpr int kMargin = 1;
    const float ox = -std::floor(min_x) + kMargin;
    const float oy = -std::floor(min_y) + kMargin;
    int w = static_cast<int>(std::ceil(max_x - std::floor(min_x))) + 2 * kMargin;
    int h = static_cast<int>(std::ceil(max_y - std::floor(min_y))) + 2 * kMargin;
    LV_ASSERT_MSG(w <= kHeadMaskMaxSide && h <= kHeadMaskMaxSide,
                  "arrowhead mask too large for this display; raise kHeadMaskMaxSide");
    if (w > kHeadMaskMaxSide) w = kHeadMaskMaxSide;
    if (h > kHeadMaskMaxSide) h = kHeadMaskMaxSide;

    nav_pt_t local[NAV_ARROWHEAD_VERT_COUNT];
    for (int i = 0; i < NAV_ARROWHEAD_VERT_COUNT; ++i) {
        local[i] = {NAV_ARROWHEAD_PERIMETER[i].x * scale + ox,
                    NAV_ARROWHEAD_PERIMETER[i].y * scale + oy};
    }
    std::memset(head_mask_, 0, sizeof(head_mask_));
    rasterize_poly_a8(local, NAV_ARROWHEAD_VERT_COUNT, w, h, head_mask_);

    // The tip is authored at the origin, so it lands exactly on the offset.
    head_pivot_ = {NAV_ARROWHEAD_TIP.x * scale + ox, NAV_ARROWHEAD_TIP.y * scale + oy};

    head_img_.header.magic = LV_IMAGE_HEADER_MAGIC;
    head_img_.header.cf = LV_COLOR_FORMAT_A8;
    head_img_.header.w = static_cast<uint32_t>(w);
    head_img_.header.h = static_cast<uint32_t>(h);
    head_img_.header.stride = static_cast<uint32_t>(w);
    head_img_.data = head_mask_;
    head_img_.data_size = static_cast<uint32_t>(w * h);
}

float NavRenderer::px_per_unit() const {
    return static_cast<float>(display_diameter_px_) / kDesignDiameter;
}

float NavRenderer::start_dist() const {
    return route_count_ ? route_[0].start_dist : 0.0f;
}

float NavRenderer::total_dist() const {
    return route_count_ ? route_[route_count_ - 1].end_dist : 0.0f;
}

void NavRenderer::append_segments(const nav_segment_t *segs, int count, const Transform &t) {
    // The shipped data's distances restart at 0 per maneuver; rebase them onto the running route.
    const float base = total_dist();
    for (int i = 0; i < count; ++i) {
        if (route_count_ >= kRouteCapacity) break;
        const nav_segment_t &src = segs[i];
        Seg &d = route_[route_count_++];
        d.type = src.type;
        d.p0 = transform_point(src.p0, t);
        // The placement transform is a rotation + translation, so it rotates headings by exactly
        // t.rotation and leaves radius/sweep (and therefore length) alone.
        d.h0 = src.heading0 * kPi / 180.0f + t.rotation;
        d.length = src.length;
        d.radius = src.radius;
        d.turn = src.turn_deg * kPi / 180.0f;
        d.start_dist = base + src.start_dist;
        d.end_dist = base + src.end_dist;
    }
}

int NavRenderer::index_at_dist(float dist) const {
    for (int i = 0; i < route_count_; ++i) {
        if (dist <= route_[i].end_dist) return i;
    }
    return route_count_ - 1;
}

void NavRenderer::eval_segment(const Seg &s, float dist, nav_pt_t *out_point, float *out_heading) {
    const float t = s.length > 0.0f ? clampf((dist - s.start_dist) / s.length, 0.0f, 1.0f) : 0.0f;
    if (s.type == NAV_SEG_LINE) {
        *out_point = {s.p0.x + std::cos(s.h0) * s.length * t,
                      s.p0.y + std::sin(s.h0) * s.length * t};
        *out_heading = s.h0;
        return;
    }
    // Circle centre sits one radius off the start point, on the side the curve bends toward; the
    // point at heading h is then that same radial offset taken backwards from the centre (this is
    // arc_points() in build_icons.py, one sample at a time).
    const float sign = s.turn >= 0.0f ? 1.0f : -1.0f;
    const float cx = s.p0.x + sign * s.radius * (-std::sin(s.h0));
    const float cy = s.p0.y + sign * s.radius * (std::cos(s.h0));
    const float h = s.h0 + s.turn * t;
    *out_point = {cx - sign * s.radius * (-std::sin(h)), cy - sign * s.radius * (std::cos(h))};
    *out_heading = h;
}

void NavRenderer::point_and_heading_at_dist(float dist, nav_pt_t *out_point,
                                             float *out_heading) const {
    if (route_count_ == 0) {
        *out_point = {0.0f, 0.0f};
        *out_heading = -kPi / 2.0f;
        return;
    }
    dist = clampf(dist, start_dist(), total_dist());
    eval_segment(route_[index_at_dist(dist)], dist, out_point, out_heading);
}

NavRenderer::Seg NavRenderer::clip_segment(const Seg &s, float from_dist, float to_dist) {
    Seg c = s;
    const float lo = std::max(from_dist, s.start_dist);
    const float hi = std::min(to_dist, s.end_dist);
    if (hi <= lo || s.length <= 0.0f) {
        c.length = 0.0f;
        c.turn = 0.0f;
        return c;
    }
    float h;
    eval_segment(s, lo, &c.p0, &h);
    c.h0 = h;
    // Arc length is linear in sweep at constant radius, so the sweep trims by the same fraction
    // the length does -- no re-solving of the circle needed, the centre is unchanged.
    c.turn = s.turn * ((hi - lo) / s.length);
    c.length = hi - lo;
    c.start_dist = lo;
    c.end_dist = hi;
    return c;
}

int NavRenderer::slice_window(float from_dist, float to_dist, Seg *out, int max_out) const {
    from_dist = clampf(from_dist, start_dist(), total_dist());
    to_dist = clampf(to_dist, from_dist, total_dist());
    int n = 0;
    // Epsilon: base_dist eases asymptotically onto a maneuver join, and a segment clipped to a
    // hair's length there would flicker its join disc on and off frame to frame.
    constexpr float kEps = 1e-3f;
    for (int i = 0; i < route_count_ && n < max_out; ++i) {
        const Seg &s = route_[i];
        if (s.end_dist <= from_dist + kEps || s.start_dist >= to_dist - kEps) continue;
        const Seg c = clip_segment(s, from_dist, to_dist);
        if (c.length > kEps) out[n++] = c;
    }
    return n;
}

void NavRenderer::prune_before(float dist) {
    if (route_count_ < 2) return;
    const int keep = index_at_dist(dist); // the segment straddling `dist` must survive
    if (keep <= 0) return;
    const int remaining = route_count_ - keep;
    std::memmove(route_, route_ + keep, sizeof(Seg) * remaining);
    route_count_ = remaining;
}

NavRenderer::Transform NavRenderer::compute_transform(const nav_segment_t &first) const {
    const nav_pt_t base0 = first.p0;
    const float heading_new = first.heading0 * kPi / 180.0f;
    Transform t{0.0f, base0, base0};
    if (route_count_ > 0) {
        // Butt the new maneuver onto the end of the old one, matching its exit heading exactly --
        // with primitives that heading is carried, not re-derived from the last two sample points.
        const Seg &last = route_[route_count_ - 1];
        nav_pt_t tip_prev;
        float tip_heading;
        eval_segment(last, last.end_dist, &tip_prev, &tip_heading);
        t.rotation = tip_heading - heading_new;
        t.anchor = tip_prev;
    }
    return t;
}

nav_pt_t NavRenderer::transform_point(const nav_pt_t &p, const Transform &t) {
    const nav_pt_t r = rotate_point(p, t.base0, t.rotation);
    return {r.x + (t.anchor.x - t.base0.x), r.y + (t.anchor.y - t.base0.y)};
}

NavRenderer::Pose NavRenderer::compute_pose(const nav_icon_data_t &data, const Transform &t) const {
    const float entry_heading = data.main_segments[0].heading0 * kPi / 180.0f + t.rotation;
    const nav_pt_t local = data.frame_center;
    const nav_pt_t center = transform_point(local, t);
    Pose pose;
    pose.cx = center.x;
    pose.cy = center.y;
    pose.rot = -kPi / 2.0f - entry_heading;
    return pose;
}

// tween_start_ms_ is latched lazily on tick()'s first call after this, since retarget() has no
// access to the monotonic clock the caller (DialScreen) reads.
void NavRenderer::retarget(const Pose &target, uint32_t duration_ms) {
    from_ = cur_;
    to_ = target;
    to_.rot = from_.rot + norm_angle(target.rot - from_.rot);
    tween_start_ms_ = 0;
    tween_dur_ms_ = duration_ms;
    running_ = true;
}

void NavRenderer::add_maneuver(nav_render_icon_t icon, bool animate) {
    const nav_icon_data_t &data = NAV_ICON_DATA[icon];
    const Transform t = compute_transform(data.main_segments[0]);

    const float start_dist = total_dist();
    append_segments(data.main_segments, static_cast<int>(data.main_segment_count), t);
    const float end_dist = total_dist();

    const Pose pose = compute_pose(data, t);
    Pose target = pose;
    target.base_dist = start_dist;
    target.head_dist = end_dist;

    if (!animate) {
        cur_ = target;
        running_ = false;
        has_route_ = true;
        return;
    }

    const float duration =
        clampf((end_dist - start_dist) / kRevealSpeed, kMinRevealMs, kMaxRevealMs);
    retarget(target, static_cast<uint32_t>(duration));
    has_route_ = true;
}

void NavRenderer::enter_maneuver(nav_render_icon_t icon, uint32_t duration_ms) {
    route_count_ = 0;
    const nav_icon_data_t &straight = NAV_ICON_DATA[NAV_RENDER_STRAIGHT];
    for (int i = 0; i < kEntryRunwayStraights; ++i) {
        append_segments(straight.main_segments, static_cast<int>(straight.main_segment_count),
                        compute_transform(straight.main_segments[0]));
    }
    const float runway_end = total_dist();

    const nav_icon_data_t &data = NAV_ICON_DATA[icon];
    const Transform t = compute_transform(data.main_segments[0]);
    append_segments(data.main_segments, static_cast<int>(data.main_segment_count), t);

    Pose target = compute_pose(data, t);
    target.base_dist = runway_end;
    target.head_dist = total_dist();

    // Same camera, zero-length arrow parked at the far end of the runway, well below the panel;
    // the tween then runs it up the straights and onto the page.
    cur_ = target;
    cur_.base_dist = 0.0f;
    cur_.head_dist = 0.0f;
    retarget(target, duration_ms);
    has_route_ = true;
}

bool NavRenderer::tick(uint32_t now_ms) {
    if (!running_) return false;
    if (tween_start_ms_ == 0) tween_start_ms_ = now_ms;

    const uint32_t elapsed = now_ms - tween_start_ms_;
    const float t = tween_dur_ms_ > 0 ? clampf(static_cast<float>(elapsed) /
                                                    static_cast<float>(tween_dur_ms_),
                                                0.0f, 1.0f)
                                       : 1.0f;
    const float e = ease_in_out_cubic(t);
    cur_.base_dist = lerpf(from_.base_dist, to_.base_dist, e);
    cur_.head_dist = lerpf(from_.head_dist, to_.head_dist, e);
    cur_.cx = lerpf(from_.cx, to_.cx, e);
    cur_.cy = lerpf(from_.cy, to_.cy, e);
    cur_.rot = lerpf(from_.rot, to_.rot, e);

    if (t >= 1.0f) {
        running_ = false;
        cur_ = to_;
        prune_before(cur_.base_dist);
    }
    return true;
}

void NavRenderer::set_north_heading(float heading_rad, bool known) {
    compass_.set_north(heading_rad, known);
}

nav_pt_t NavRenderer::world_to_screen(const nav_pt_t &p, const Pose &cam, float basis_c,
                                       float basis_s, const lv_area_t &coords) const {
    const float dx = p.x - cam.cx, dy = p.y - cam.cy;
    const float rx = dx * basis_c - dy * basis_s;
    const float ry = dx * basis_s + dy * basis_c;
    const float sx = rx * kDisplayScale;
    const float sy = (ry + kVerticalOffset) * kDisplayScale;
    const float half = static_cast<float>(display_diameter_px_) / 2.0f;
    const float px_unit = px_per_unit();
    return {static_cast<float>(coords.x1) + half + sx * px_unit,
            static_cast<float>(coords.y1) + half + sy * px_unit};
}

// The camera transform (world_to_screen) is a similarity -- rotate, uniform scale, translate, no
// shear -- so it maps circles to circles and lines to lines. That is what lets an arc authored in
// maneuver space stay a true circular arc on screen: its centre transforms as a point, its radius
// scales uniformly, and both its angles simply shift by the camera rotation.
void NavRenderer::draw_segment(lv_layer_t *layer, const Seg &s, const Pose &cam, float basis_c,
                                float basis_s, const lv_area_t &coords, float width_px,
                                const nav_pt_t *anchor, nav_pt_t *out_end) const {
    if (s.length <= 0.0f) return;
    if (s.type == NAV_SEG_LINE) {
        const nav_pt_t b = {s.p0.x + std::cos(s.h0) * s.length,
                            s.p0.y + std::sin(s.h0) * s.length};
        const nav_pt_t sa = world_to_screen(s.p0, cam, basis_c, basis_s, coords);
        const nav_pt_t sb = world_to_screen(b, cam, basis_c, basis_s, coords);
        stroke_line(layer, sa.x, sa.y, sb.x, sb.y, width_px, route_color(), /*rounded=*/true,
                    anchor, out_end);
        return;
    }
    const float sign = s.turn >= 0.0f ? 1.0f : -1.0f;
    const nav_pt_t center = {s.p0.x + sign * s.radius * (-std::sin(s.h0)),
                             s.p0.y + sign * s.radius * (std::cos(s.h0))};
    const nav_pt_t center_screen = world_to_screen(center, cam, basis_c, basis_s, coords);
    // Where this arc's start point lands on screen -- the same point the preceding straight drew
    // its endpoint at, and the anchor stroke_arc() hangs the whole curve off.
    const nav_pt_t ideal_start = world_to_screen(s.p0, cam, basis_c, basis_s, coords);
    const nav_pt_t start_screen =
        anchor != nullptr ? *anchor : nav_pt_t{std::round(ideal_start.x), std::round(ideal_start.y)};
    const float radius_px = s.radius * kDisplayScale * px_per_unit();

    // A point's angle about the centre runs a quarter turn behind/ahead of the travel heading
    // (which side depends on which way the curve bends), and advances with it; LVGL measures
    // angles the same way atan2 does here (0 = +x, increasing toward +y), so no axis fixups.
    const float sweep = std::fabs(deg_of(s.turn));
    // lv_draw_arc only sweeps start -> end in increasing degrees, so for a left-bending curve the
    // start is the far end of the same sweep.
    float a0 = deg_of(s.h0 - sign * kPi / 2.0f + cam.rot) - (s.turn < 0.0f ? sweep : 0.0f);
    a0 = std::fmod(a0, 360.0f);
    if (a0 < 0.0f) a0 += 360.0f;
    stroke_arc(layer, center_screen, start_screen, radius_px, a0, a0 + sweep, width_px,
               route_color(), /*path_end_at_start=*/s.turn < 0.0f, out_end);
}

void NavRenderer::draw(lv_layer_t *layer, const lv_area_t &coords, float ring_scale) const {
    compass_.draw(layer, coords, ring_scale);
    draw_route_only(layer, coords);
}

void NavRenderer::draw_route_only(lv_layer_t *layer, const lv_area_t &coords) const {
    if (!has_route_) return;

    const float basis_c = std::cos(cur_.rot), basis_s = std::sin(cur_.rot);
    const auto to_screen = [&](const nav_pt_t &p) {
        return world_to_screen(p, cur_, basis_c, basis_s, coords);
    };

    Seg window[kRouteCapacity];
    const float trimmed_head = std::max(cur_.base_dist, cur_.head_dist - kHeadDepth);
    const int window_n = slice_window(cur_.base_dist, trimmed_head, window, kRouteCapacity);
    const float width_px = kLineThickness * kDisplayScale * px_per_unit();
    // Chain each segment onto the pixel its predecessor actually finished on, rather than onto the
    // ideal float joint. Anchoring every segment to the ideal point fixed straight-to-curve, but
    // left curve-to-curve still visibly mismatched: a curve's drawn end is
    // `integer centre + integer radius` at a whole-degree angle, which is not the rounded ideal
    // joint, so the next curve hung itself off a point the previous one never reached. The
    // roundabouts chain four arcs and so showed it worst.
    nav_pt_t joint;
    bool have_joint = false;
    for (int i = 0; i < window_n; ++i) {
        nav_pt_t end;
        draw_segment(layer, window[i], cur_, basis_c, basis_s, coords, width_px,
                     have_joint ? &joint : nullptr, &end);
        joint = end;
        have_joint = true;
    }
    // No join discs. An earlier pass stamped one at each interior joint to cover the step in the
    // silhouette where a line and an arc had been quantised independently; stroke_arc() now hangs
    // each curve off the point its neighbour actually drew, so there is no step left to cover, and
    // a disc has the same rounded-centre problem it was meant to hide.

    nav_pt_t head_point;
    float head_heading;
    point_and_heading_at_dist(cur_.head_dist, &head_point, &head_heading);

    // The glyph is already rasterized at final size and tip-at-origin (build_arrowhead_mask), so
    // all that is left per frame is to rotate it from its authored "up" orientation to the travel
    // heading and pin its tip to the route end -- one lv_draw_image, no geometry, no seams.
    // head_heading is a WORLD heading; the camera adds its own rotation on top (world_to_screen
    // used to supply it, back when the glyph's vertices went through that transform too).
    const float local_forward = -kPi / 2.0f;
    const float rotation = head_heading - local_forward + cur_.rot;
    const nav_pt_t tip_screen = to_screen(head_point);

    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.src = &head_img_;
    dsc.opa = LV_OPA_COVER;
    // An A8 source carries coverage only; `recolor` is the colour LVGL blends through it.
    dsc.recolor = route_color();
    dsc.recolor_opa = LV_OPA_COVER;
    dsc.antialias = true;
    dsc.pivot = {static_cast<int32_t>(std::lround(head_pivot_.x)),
                 static_cast<int32_t>(std::lround(head_pivot_.y))};
    // lv_draw_image takes rotation in 0.1 degree units, positive clockwise -- the same sense as
    // screen-space `rotation` here, since y points down.
    int32_t rot_tenths = static_cast<int32_t>(std::lround(deg_of(rotation) * 10.0f)) % 3600;
    if (rot_tenths < 0) rot_tenths += 3600;
    dsc.rotation = rot_tenths;

    // Placing the image so its pivot lands on the tip keeps the tip fixed under rotation.
    lv_area_t head_area;
    head_area.x1 = static_cast<int32_t>(std::lround(tip_screen.x - head_pivot_.x));
    head_area.y1 = static_cast<int32_t>(std::lround(tip_screen.y - head_pivot_.y));
    head_area.x2 = head_area.x1 + static_cast<int32_t>(head_img_.header.w) - 1;
    head_area.y2 = head_area.y1 + static_cast<int32_t>(head_img_.header.h) - 1;
    lv_draw_image(layer, &dsc, &head_area);
}
