#include "nav_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDesignDiameter = 600.0f; // maneuvers.json / nav_icons_data.h authoring circle

// --- ported knobs (Demo/params.js) ----------------------------------------------------------
constexpr float kDisplayScale = 1.1f;
constexpr float kLineThickness = 22.0f;     // route units, before displayScale
constexpr float kArrowheadScale = 0.25f;
constexpr float kVerticalOffset = 1.0f;     // route units, screen space
constexpr float kSeamOverlap = 0.5f;        // px
constexpr float kRevealSpeed = 0.9f;        // route units / ms
constexpr uint32_t kMinRevealMs = 1000;
constexpr uint32_t kMaxRevealMs = 1200;

constexpr int kCompassTickCount = 36;
constexpr int kCompassMajorEvery = 3;
constexpr float kCompassMinorLength = 0.06f;
constexpr float kCompassMajorLength = 0.10f;
constexpr float kCompassMinorWidth = 3.0f;
constexpr float kCompassMajorWidth = 6.0f;
constexpr float kCompassNorthLength = 0.10f;
constexpr float kCompassNorthWidth = 0.045f;

// HEAD_DEPTH (Demo/demo.js): how far back from the arrowhead's tip its glyph is already at full
// width -- the rendered line is trimmed short by this much so it tucks under the glyph.
const float kHeadDepth = NAV_ARROWHEAD_HEAD_BASE_DEPTH * kArrowheadScale;

lv_color_t compass_color() { return lv_color_hex(0x585f68); }
lv_color_t north_color() { return lv_color_hex(0xff3b30); }
lv_color_t route_color() { return lv_color_white(); }

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float lerpf(float a, float b, float t) { return a + (b - a) * t; }

float ease_in_out_cubic(float t) {
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f;
}

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
    dsc.center = {static_cast<int32_t>(cx), static_cast<int32_t>(cy)};
    dsc.radius = static_cast<uint16_t>(r);
    dsc.width = static_cast<int32_t>(r);
    dsc.start_angle = 0;
    dsc.end_angle = 360;
    lv_draw_arc(layer, &dsc);
}

void stroke_line(lv_layer_t *layer, float x1, float y1, float x2, float y2, float width,
                  lv_color_t color, bool rounded = false) {
    if (width <= 0.0f) return;
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.p1 = {static_cast<lv_value_precise_t>(x1), static_cast<lv_value_precise_t>(y1)};
    dsc.p2 = {static_cast<lv_value_precise_t>(x2), static_cast<lv_value_precise_t>(y2)};
    dsc.color = color;
    dsc.width = static_cast<int32_t>(width);
    dsc.round_start = rounded;
    dsc.round_end = rounded;
    lv_draw_line(layer, &dsc);
}

// A curve is one native LVGL arc -- the reason this renderer carries primitives rather than
// sampled points. lv_draw_arc fills the band [radius - width, radius], so `radius` is pushed out
// by half the stroke to centre the band on the true centreline. Rounded ends match
// lv_draw_line's rounded caps so a straight-to-curve join closes without a separate disc.
void stroke_arc(lv_layer_t *layer, const nav_pt_t &center, float radius, float start_deg,
                 float end_deg, float width, lv_color_t color) {
    if (radius <= 0.0f || width <= 0.0f || end_deg <= start_deg) return;
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.color = color;
    dsc.opa = LV_OPA_COVER;
    dsc.center = {static_cast<int32_t>(std::lround(center.x)),
                  static_cast<int32_t>(std::lround(center.y))};
    dsc.radius = static_cast<uint16_t>(std::lround(radius + width / 2.0f));
    dsc.width = static_cast<int32_t>(std::lround(width));
    dsc.rounded = 1;
    dsc.start_angle = static_cast<lv_value_precise_t>(start_deg);
    dsc.end_angle = static_cast<lv_value_precise_t>(end_deg);
    lv_draw_arc(layer, &dsc);
}

float deg_of(float rad) { return rad * 180.0f / kPi; }

} // namespace

NavRenderer::NavRenderer(int32_t display_diameter_px) : display_diameter_px_(display_diameter_px) {
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
    north_heading_rad_ = heading_rad;
    north_known_ = known;
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
                                float basis_s, const lv_area_t &coords, float width_px) const {
    if (s.length <= 0.0f) return;
    if (s.type == NAV_SEG_LINE) {
        const nav_pt_t b = {s.p0.x + std::cos(s.h0) * s.length,
                            s.p0.y + std::sin(s.h0) * s.length};
        const nav_pt_t sa = world_to_screen(s.p0, cam, basis_c, basis_s, coords);
        const nav_pt_t sb = world_to_screen(b, cam, basis_c, basis_s, coords);
        stroke_line(layer, sa.x, sa.y, sb.x, sb.y, width_px, route_color(), /*rounded=*/true);
        return;
    }
    const float sign = s.turn >= 0.0f ? 1.0f : -1.0f;
    const nav_pt_t center = {s.p0.x + sign * s.radius * (-std::sin(s.h0)),
                             s.p0.y + sign * s.radius * (std::cos(s.h0))};
    const nav_pt_t center_screen = world_to_screen(center, cam, basis_c, basis_s, coords);
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
    stroke_arc(layer, center_screen, radius_px, a0, a0 + sweep, width_px, route_color());
}

void NavRenderer::draw_compass_ring(lv_layer_t *layer, const lv_area_t &coords) const {
    const float half = static_cast<float>(display_diameter_px_) / 2.0f;
    const float cx = static_cast<float>(coords.x1) + half;
    const float cy = static_cast<float>(coords.y1) + half;
    const float px_unit = px_per_unit();

    for (int i = 0; i < kCompassTickCount; ++i) {
        const float angle =
            (static_cast<float>(i) / kCompassTickCount) * 2.0f * kPi - kPi / 2.0f;
        const bool is_major = (i % kCompassMajorEvery) == 0;
        const float len = (is_major ? kCompassMajorLength : kCompassMinorLength) * half;
        const float w = std::max(1.0f, (is_major ? kCompassMajorWidth : kCompassMinorWidth) *
                                            px_unit);
        const float c = std::cos(angle), s = std::sin(angle);
        stroke_line(layer, cx + c * half, cy + s * half, cx + c * (half - len),
                    cy + s * (half - len), w, compass_color());
    }

    if (!north_known_) return;
    // The route is drawn heading-up: the bike's forward direction is pinned to the top of the
    // screen, so the world -- north included -- rotates the OPPOSITE way to the heading. Hence the
    // negation. Without it the marker sweeps at exactly the right rate in exactly the wrong
    // direction, which is what "the compass spins backwards" on the 2026-09-23 ride was.
    // (-kPi/2 then converts "clockwise from up" to the atan2 convention, 0 = +x.)
    const float screen_angle = -north_heading_rad_ - kPi / 2.0f;
    const float nc = std::cos(screen_angle), ns = std::sin(screen_angle);
    const float tip_r = half * (1.0f - kCompassNorthLength);
    const nav_pt_t tip = {cx + nc * tip_r, cy + ns * tip_r};
    const float base_half_w = kCompassNorthWidth * half;
    const float perp_x = -ns, perp_y = nc;
    const nav_pt_t base_a = {cx + nc * half + perp_x * base_half_w,
                              cy + ns * half + perp_y * base_half_w};
    const nav_pt_t base_b = {cx + nc * half - perp_x * base_half_w,
                              cy + ns * half - perp_y * base_half_w};
    fill_triangle(layer, base_a, base_b, tip, north_color());
}

void NavRenderer::draw(lv_layer_t *layer, const lv_area_t &coords) const {
    if (!has_route_) return;

    draw_compass_ring(layer, coords);

    const float basis_c = std::cos(cur_.rot), basis_s = std::sin(cur_.rot);
    const auto to_screen = [&](const nav_pt_t &p) {
        return world_to_screen(p, cur_, basis_c, basis_s, coords);
    };

    Seg window[kRouteCapacity];
    const float trimmed_head = std::max(cur_.base_dist, cur_.head_dist - kHeadDepth);
    const int window_n = slice_window(cur_.base_dist, trimmed_head, window, kRouteCapacity);
    const float width_px = kLineThickness * kDisplayScale * px_per_unit();
    for (int i = 0; i < window_n; ++i) {
        draw_segment(layer, window[i], cur_, basis_c, basis_s, coords, width_px);
    }
    // No join discs: consecutive segments are tangent and both ends are rounded, so each cap
    // lands inside its neighbour's stroke.

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
