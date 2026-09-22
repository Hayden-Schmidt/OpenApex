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

float heading_of(const nav_pt_t &from, const nav_pt_t &to) {
    return std::atan2(to.y - from.y, to.x - from.x);
}

lv_point_precise_t to_lv(const nav_pt_t &p) {
    return {static_cast<lv_value_precise_t>(p.x), static_cast<lv_value_precise_t>(p.y)};
}

// -- LVGL-shaped drawing primitives (C++ twin of Demo/lvgl_shim.js) --------------------------

// Offsets a convex polygon (<=4 points here) outward by `d` so adjacent filled pieces overlap
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
    Line lines[4];
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

// LVGL has no quad fill primitive (unlike lv_canvas_draw_polygon(4) in the JS shim comment) --
// every quad here is convex by construction (a stroked segment's offset rectangle), so it's
// split into two triangles sharing the a-c diagonal. That diagonal doesn't exist in the JS shim
// at all (Canvas fills the whole quad as one polygon) -- it's a seam this port introduces, and it
// needs the exact same per-triangle inflate() growth fillTriangulated() already uses for the
// arrowhead's triangle-to-triangle seams, or the two halves show a hairline down the diagonal.
void fill_convex_quad(lv_layer_t *layer, const nav_pt_t &a, const nav_pt_t &b, const nav_pt_t &c,
                       const nav_pt_t &d, lv_color_t color, float grow = 0.0f) {
    fill_triangle(layer, a, b, c, color, grow);
    fill_triangle(layer, a, c, d, color, grow);
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
                  lv_color_t color) {
    if (width <= 0.0f) return;
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.p1 = {static_cast<lv_value_precise_t>(x1), static_cast<lv_value_precise_t>(y1)};
    dsc.p2 = {static_cast<lv_value_precise_t>(x2), static_cast<lv_value_precise_t>(y2)};
    dsc.color = color;
    dsc.width = static_cast<int32_t>(width);
    dsc.round_start = 0;
    dsc.round_end = 0;
    lv_draw_line(layer, &dsc);
}

// One quad per segment (offset by the segment's normal) plus a disc at each interior vertex, so
// bends read as round joins without LVGL's line draw needing one -- see lvgl_shim.js's
// strokePolyline() note.
void stroke_polyline(lv_layer_t *layer, const nav_pt_t *pts, int count, float width,
                      lv_color_t color, float grow) {
    if (count < 2 || width <= 0.0f) return;
    const float h = width / 2.0f;
    for (int i = 1; i < count; ++i) {
        const nav_pt_t &a = pts[i - 1];
        const nav_pt_t &b = pts[i];
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-9f) continue;
        const float nx = (-dy / len) * h, ny = (dx / len) * h;
        fill_convex_quad(layer, {a.x + nx, a.y + ny}, {b.x + nx, b.y + ny}, {b.x - nx, b.y - ny},
                          {a.x - nx, a.y - ny}, color, grow);
    }
    // TODO: disc joins disabled -- flickers at the route's base during maneuver transitions
    // (interior vertex near cur_.base_dist toggling in/out of the render window frame-to-frame).
    // for (int i = 1; i < count - 1; ++i) {
    //     fill_circle(layer, pts[i].x, pts[i].y, h + grow, color);
    // }
}

} // namespace

NavRenderer::NavRenderer(int32_t display_diameter_px) : display_diameter_px_(display_diameter_px) {}

float NavRenderer::px_per_unit() const {
    return static_cast<float>(display_diameter_px_) / kDesignDiameter;
}

float NavRenderer::total_dist() const {
    return route_count_ ? cum_dist_[route_count_ - 1] : 0.0f;
}

void NavRenderer::append_points(const nav_pt_t *pts, int count) {
    const int start = route_count_ ? 1 : 0;
    for (int i = start; i < count; ++i) {
        if (route_count_ >= kRouteCapacity) break;
        const nav_pt_t &p = pts[i];
        float d = 0.0f;
        if (route_count_ > 0) {
            const nav_pt_t &prev = route_points_[route_count_ - 1];
            const float dx = p.x - prev.x, dy = p.y - prev.y;
            d = std::sqrt(dx * dx + dy * dy);
        }
        const float cum = (route_count_ ? cum_dist_[route_count_ - 1] : 0.0f) + d;
        route_points_[route_count_] = p;
        cum_dist_[route_count_] = cum;
        ++route_count_;
    }
}

int NavRenderer::index_at_dist(float dist) const {
    int lo = 1, hi = route_count_ - 1;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (cum_dist_[mid] < dist) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

void NavRenderer::point_and_heading_at_dist(float dist, nav_pt_t *out_point,
                                             float *out_heading) const {
    if (route_count_ < 2) {
        *out_point = route_count_ ? route_points_[0] : nav_pt_t{0.0f, 0.0f};
        *out_heading = -kPi / 2.0f;
        return;
    }
    dist = clampf(dist, cum_dist_[0], total_dist());
    const int i = index_at_dist(dist);
    const nav_pt_t &a = route_points_[i - 1];
    const nav_pt_t &b = route_points_[i];
    const float seg_start = cum_dist_[i - 1], seg_end = cum_dist_[i];
    const float seg_len = seg_end - seg_start;
    const float t = seg_len > 0.0f ? (dist - seg_start) / seg_len : 0.0f;
    *out_point = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
    *out_heading = heading_of(a, b);
}

int NavRenderer::slice_window(float from_dist, float to_dist, nav_pt_t *out, int max_out) const {
    const float total = total_dist();
    const float lo = route_count_ ? cum_dist_[0] : 0.0f;
    from_dist = clampf(from_dist, lo, total);
    to_dist = clampf(to_dist, lo, total);
    int n = 0;
    nav_pt_t p;
    float h;
    point_and_heading_at_dist(from_dist, &p, &h);
    if (n < max_out) out[n++] = p;
    if (to_dist > from_dist) {
        // Epsilon margin: near a transition's end, from_dist eases asymptotically toward a real
        // route vertex's exact cum_dist (the old/new maneuver join point). A bare `>` test flips
        // in and out of inclusion across consecutive frames as float rounding wobbles around that
        // near-equality, producing a near-zero-length leading segment whose disc flickers on/off
        // at the base of the line. The margin keeps that vertex merged into the interpolated start
        // point instead.
        constexpr float kEps = 1e-3f;
        for (int i = index_at_dist(from_dist); i < route_count_ && cum_dist_[i] < to_dist; ++i) {
            if (cum_dist_[i] > from_dist + kEps && n < max_out) out[n++] = route_points_[i];
        }
        point_and_heading_at_dist(to_dist, &p, &h);
        if (n < max_out) out[n++] = p;
    }
    return n;
}

void NavRenderer::prune_before(float dist) {
    if (route_count_ < 3 || dist <= cum_dist_[0]) return;
    const int keep = index_at_dist(dist) - 1; // segment straddling `dist` must survive
    if (keep <= 0) return;
    const int remaining = route_count_ - keep;
    std::memmove(route_points_, route_points_ + keep, sizeof(nav_pt_t) * remaining);
    std::memmove(cum_dist_, cum_dist_ + keep, sizeof(float) * remaining);
    route_count_ = remaining;
}

NavRenderer::Transform NavRenderer::compute_transform(const nav_pt_t local_main[2]) const {
    const nav_pt_t &base0 = local_main[0];
    const nav_pt_t &base1 = local_main[1];
    const float heading_new = heading_of(base0, base1);
    Transform t{0.0f, base0, base0};
    if (route_count_ > 0) {
        const nav_pt_t &tip_prev = route_points_[route_count_ - 1];
        const nav_pt_t &pre_tip_prev =
            route_count_ > 1 ? route_points_[route_count_ - 2] : tip_prev;
        t.rotation = heading_of(pre_tip_prev, tip_prev) - heading_new;
        t.anchor = tip_prev;
    }
    return t;
}

nav_pt_t NavRenderer::transform_point(const nav_pt_t &p, const Transform &t) {
    const nav_pt_t r = rotate_point(p, t.base0, t.rotation);
    return {r.x + (t.anchor.x - t.base0.x), r.y + (t.anchor.y - t.base0.y)};
}

NavRenderer::Pose NavRenderer::compute_pose(const nav_icon_data_t &data, const Transform &t) const {
    const float entry_heading =
        heading_of(transform_point(data.main_path[0], t), transform_point(data.main_path[1], t));
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
    nav_pt_t local_main[2] = {data.main_path[0], data.main_path[1]};
    const Transform t = compute_transform(local_main);

    // Transform every authored point (not just the two used for the transform above).
    nav_pt_t world_main[kRouteCapacity];
    const int n = std::min<int>(data.main_count, kRouteCapacity);
    for (int i = 0; i < n; ++i) world_main[i] = transform_point(data.main_path[i], t);

    const float start_dist = total_dist();
    append_points(world_main, n);
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
    const float screen_angle = north_heading_rad_ - kPi / 2.0f;
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

    nav_pt_t window[kRouteCapacity];
    const float trimmed_head = std::max(cur_.base_dist, cur_.head_dist - kHeadDepth);
    const int window_n = slice_window(cur_.base_dist, trimmed_head, window, kRouteCapacity);
    nav_pt_t window_screen[kRouteCapacity];
    for (int i = 0; i < window_n; ++i) window_screen[i] = to_screen(window[i]);
    stroke_polyline(layer, window_screen, window_n, kLineThickness * kDisplayScale * px_per_unit(),
                     route_color(), kSeamOverlap);

    nav_pt_t head_point;
    float head_heading;
    point_and_heading_at_dist(cur_.head_dist, &head_point, &head_heading);

    // ARROWHEAD.perimeter is authored tip-at-origin, pointing "up" (-y); scale in local space
    // (so the tip stays pinned) then rotate to the travel heading and translate to the tip.
    const float local_forward = -kPi / 2.0f;
    const float rotation = head_heading - local_forward;
    const float rc = std::cos(rotation), rs = std::sin(rotation);
    nav_pt_t head_screen[NAV_ARROWHEAD_VERT_COUNT];
    for (int i = 0; i < NAV_ARROWHEAD_VERT_COUNT; ++i) {
        const nav_pt_t &v = NAV_ARROWHEAD_PERIMETER[i];
        const nav_pt_t scaled = {v.x * kArrowheadScale, v.y * kArrowheadScale};
        const nav_pt_t rotated = {scaled.x * rc - scaled.y * rs, scaled.x * rs + scaled.y * rc};
        head_screen[i] = to_screen({rotated.x + head_point.x, rotated.y + head_point.y});
    }
    for (int i = 0; i < NAV_ARROWHEAD_TRI_COUNT; ++i) {
        const uint8_t *tri = NAV_ARROWHEAD_TRIS[i];
        fill_triangle(layer, head_screen[tri[0]], head_screen[tri[1]], head_screen[tri[2]],
                      route_color(), kSeamOverlap);
    }
}
