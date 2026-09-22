#pragma once

#include <cstdint>

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
    // Fixed capacity: the largest single maneuver is 63 points (roundabout_right); a transition
    // briefly holds the tail of the previous maneuver plus the new one, so 2x that with margin
    // covers every case without a heap allocation ever happening here.
    static constexpr int kRouteCapacity = 160;
    nav_pt_t route_points_[kRouteCapacity];
    float cum_dist_[kRouteCapacity];
    int route_count_ = 0;

    float total_dist() const;
    void append_points(const nav_pt_t *pts, int count);
    int index_at_dist(float dist) const;
    void point_and_heading_at_dist(float dist, nav_pt_t *out_point, float *out_heading) const;
    // Writes the rendered window into out[], returns point count (capped at max_out).
    int slice_window(float from_dist, float to_dist, nav_pt_t *out, int max_out) const;
    void prune_before(float dist);

    Transform compute_transform(const nav_pt_t local_main[2]) const;
    static nav_pt_t transform_point(const nav_pt_t &p, const Transform &t);
    Pose compute_pose(const nav_icon_data_t &data, const Transform &t) const;

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
    float north_heading_rad_ = 0.0f;
    bool north_known_ = false;

    // -- camera / geometry helpers ----------------------------------------------------------
    const int32_t display_diameter_px_;
    float px_per_unit() const;

    nav_pt_t world_to_screen(const nav_pt_t &p, const Pose &cam, float basis_c, float basis_s,
                              const lv_area_t &coords) const;

    void draw_compass_ring(lv_layer_t *layer, const lv_area_t &coords) const;
};
