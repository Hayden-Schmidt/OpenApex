#pragma once

#include <cstdint>

#include "compass_ring.hpp"
#include "lvgl.h"
#include "nav_icons_data.h"

// Route/camera/tween maneuver renderer -- C++ port of Demo/demo.js + Demo/lvgl_shim.js (see those
// files for the "why" behind every algorithm below; this is a structural 1:1 port, not a
// reinterpretation). Draws straight into an LVGL layer via LV_EVENT_DRAW_MAIN, no lv_canvas
// framebuffer -- see lvgl_shim.js's own LVGL-equivalents mapping comment.
//
// One instance per DialScreen. Not thread-safe; only ever touched from the GUI task.
//
// Ported knobs (Demo/params.js) are baked in as constants below rather than left runtime-
// configurable -- nothing in this firmware currently needs to change them at runtime.
class NavRenderer {
public:
    // display_diameter_px: the active display's pixel diameter (runtime lv_display resolution --
    // equal to BOARD_DISP_WIDTH on real hardware). Maneuver geometry
    // is authored against a fixed 600-unit circle (nav_icons_data.h); this is the only
    // route-units -> pixels scale factor, matching Demo/demo.js's DESIGN_DIAMETER.
    explicit NavRenderer(int32_t display_diameter_px);

    // Appends a new maneuver to the route and starts (or mid-flight retargets) the reveal+camera
    // tween. Call whenever the mapped nav_render_icon_t changes. animate=false snaps instantly
    // (used for the very first maneuver, mirroring demo.js's init()).
    void add_maneuver(nav_render_icon_t icon, bool animate);

    // Advances the tween against a monotonic millisecond clock. Returns true while a maneuver
    // transition is in flight -- callers should keep invalidating/redrawing every frame in that
    // case, and can skip redraw work entirely once it returns false (matches demo.js's "only
    // rAF while running" behavior, to keep idle CPU cost near zero on the C3).
    bool tick(uint32_t now_ms);

    // Screen-space heading the compass ring's north mark points to (radians, 0 = up, clockwise).
    // Unlike the JS demo's placeholder spin, this is driven directly from real heading data.
    void set_north_heading(float heading_rad, bool known);

    // Draws the current frame into `layer`, within the pixel square at `coords` (expected square,
    // side length == display_diameter_px passed to the constructor).
    void draw(lv_layer_t *layer, const lv_area_t &coords) const;

private:
    struct Pose {
        float base_dist = 0.0f;
        float head_dist = 0.0f;
        float cx = 0.0f;
        float cy = 0.0f;
        float rot = 0.0f;
    };

    struct Transform {
        float rotation;
        nav_pt_t anchor;
        nav_pt_t base0;
    };

    // -- route model (Demo/demo.js's routePoints/cumDist) ---------------------------------------
    // The route is a chain of EXACT primitives, not sampled points: nav_icons_data.h ships each
    // maneuver as the straights/curves maneuvers.json declares, and each is drawn whole (one quad
    // per straight, one lv_draw_arc per curve). A curve therefore has no internal joints to seam,
    // and a turn costs ~3 draws instead of ~22 quads + ~21 discs.
    //
    // Seg is the world-space, radians twin of nav_segment_t (the shipped data is local-space and
    // degrees, matching the authoring convention).
    struct Seg {
        nav_seg_type_t type;
        nav_pt_t p0;     // start point, world space
        float h0;        // travel heading at p0, radians
        float length;    // arc length for arcs
        float radius;
        float turn;      // signed sweep, radians; 0 for lines
        float start_dist; // cumulative along the whole route, not the maneuver
        float end_dist;
    };

    // Fixed capacity: the longest maneuver is NAV_ICON_MAX_SEGMENTS; a transition briefly holds the
    // tail of the previous maneuver plus the whole new one, so 2x that with margin covers every
    // case without a heap allocation ever happening here.
    static constexpr int kRouteCapacity = NAV_ICON_MAX_SEGMENTS * 2 + 4;
    Seg route_[kRouteCapacity];
    int route_count_ = 0;

    float start_dist() const;
    float total_dist() const;
    void append_segments(const nav_segment_t *segs, int count, const Transform &t);
    // Index of the segment containing `dist` (clamped to the route's ends).
    int index_at_dist(float dist) const;
    // Point/heading at a distance along one segment -- the only place arc geometry is unpacked.
    static void eval_segment(const Seg &s, float dist, nav_pt_t *out_point, float *out_heading);
    void point_and_heading_at_dist(float dist, nav_pt_t *out_point, float *out_heading) const;
    // Clips `s` to [from_dist, to_dist]; the result is just a shorter segment of the same kind
    // (arc length is linear in sweep angle at constant radius, so an arc trims proportionally).
    static Seg clip_segment(const Seg &s, float from_dist, float to_dist);
    // Writes the revealed window into out[], returns segment count (capped at max_out).
    int slice_window(float from_dist, float to_dist, Seg *out, int max_out) const;
    void prune_before(float dist);

    Transform compute_transform(const nav_segment_t &first) const;
    static nav_pt_t transform_point(const nav_pt_t &p, const Transform &t);
    Pose compute_pose(const nav_icon_data_t &data, const Transform &t) const;

    // `anchor` is the pixel the previous segment was actually drawn to (null for the first), and
    // `out_end` reports the same for this one, so joins are made against what LVGL really drew
    // rather than against the ideal float geometry both sides quantise away from.
    void draw_segment(lv_layer_t *layer, const Seg &s, const Pose &cam, float basis_c,
                      float basis_s, const lv_area_t &coords, float width_px,
                      const nav_pt_t *anchor, nav_pt_t *out_end) const;

    // -- arrowhead glyph (pre-rasterized alpha mask) ---------------------------------------------
    // The arrowhead is a concave 7-gon, and LVGL has no polygon fill: splitting it into triangles
    // makes each shared interior edge an anti-aliasing boundary, where two ~50%-covered pixels
    // composite to ~75% instead of opaque -- a permanent lighter seam no amount of outward growth
    // can close. So it isn't filled as geometry at all: the glyph is a FIXED shape at a FIXED size
    // (only its rotation changes frame to frame), so it's rasterized once here into an 8-bit
    // coverage mask and drawn each frame as a single recoloured, rotated lv_draw_image. One shape,
    // one anti-aliasing pass, no interior edges -- seams become structurally impossible.
    //
    // Sized for the largest panel this renderer is built for; the glyph's on-screen extent is
    // ~0.186 * display diameter (see build_arrowhead_mask), so 64 covers diameters up to ~344.
    static constexpr int kHeadMaskMaxSide = 64;
    uint8_t head_mask_[kHeadMaskMaxSide * kHeadMaskMaxSide];
    lv_image_dsc_t head_img_{};
    // Where the glyph's tip sits inside the mask -- both the image's placement anchor and the
    // rotation pivot, so the tip stays pinned to the route end exactly as the polygon fill did.
    nav_pt_t head_pivot_{0.0f, 0.0f};
    void build_arrowhead_mask();

    // -- tween state (Demo/demo.js's cur/from/to/tick/retarget) ---------------------------------
    Pose cur_;
    Pose from_;
    Pose to_;
    uint32_t tween_start_ms_ = 0;
    uint32_t tween_dur_ms_ = 0;
    bool running_ = false;
    bool has_route_ = false;

    void retarget(const Pose &target, uint32_t duration_ms);

    // -- compass ------------------------------------------------------------------------------
    // Shared with the odometer page; see compass_ring.hpp. Declared before display_diameter_px_ so
    // it is constructed first -- member init order follows declaration order, not the ctor list.
    CompassRing compass_;

    // -- camera / geometry helpers ----------------------------------------------------------
    const int32_t display_diameter_px_;
    float px_per_unit() const;

    nav_pt_t world_to_screen(const nav_pt_t &p, const Pose &cam, float basis_c, float basis_s,
                              const lv_area_t &coords) const;

};
