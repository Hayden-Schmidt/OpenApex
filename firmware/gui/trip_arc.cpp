#include "trip_arc.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// Geometry from "design reference/3.Turn by Turn/OpenApex Hardware Design Ref.svg", in its 240px
// reference frame: the ring is r=114.5 with a 5px stroke, centred on the screen.
constexpr float kRefHalf = 120.0f;
constexpr float kRadiusRef = 114.5f;
constexpr float kStrokeRef = 5.0f;

// Where the ring stops either side of the bottom. The design cuts the gap with a black circle of
// r=44 centred at (120, 246); these are the two angles at which that circle crosses the ring, so
// stopping the arc here is geometrically identical to the mask -- with tidier ends.
// Angles are LVGL's convention: degrees, 0 = +x (3 o'clock), increasing clockwise because y is down.
constexpr float kArcStartDeg = 110.366f; // lower left
constexpr float kArcEndDeg = 429.634f;   // lower right, once round
constexpr float kArcSweepDeg = kArcEndDeg - kArcStartDeg;

// Traffic colours. #009AA6 is the only one the design ref pins down (it draws the whole ring
// free-flowing); the rest follow Google's own notification palette by eye.
// TODO(design): confirm all four against a real notification capture -- see the drive-log procedure
// in the repo docs -- before treating these as final.
lv_color_t color_for(uint8_t level) {
    switch (level) {
        case NAV_TRAFFIC_FREE:    return lv_color_hex(0x009AA6);
        case NAV_TRAFFIC_SLOW:    return lv_color_hex(0xF5A623);
        case NAV_TRAFFIC_HEAVY:   return lv_color_hex(0xE0392B);
        case NAV_TRAFFIC_STOPPED: return lv_color_hex(0x8B1A12);
        default:                  return lv_color_hex(0x585F68); // unknown: same grey as the ticks
    }
}

} // namespace

TripArc::TripArc(int32_t display_diameter_px) : display_diameter_px_(display_diameter_px) {}

void TripArc::set_state(const terminal_view_state_t &state) {
    count_ = state.traffic_count > NAV_TRAFFIC_MAX_SPANS ? NAV_TRAFFIC_MAX_SPANS
                                                          : state.traffic_count;
    std::memcpy(spans_, state.traffic, sizeof(nav_traffic_span_t) * count_);
    progress_permille_ = state.trip_progress_permille > 1000 ? 1000 : state.trip_progress_permille;
}

void TripArc::draw(lv_layer_t *layer, const lv_area_t &coords) const {
    const float scale = static_cast<float>(display_diameter_px_) / (kRefHalf * 2.0f);
    const float half = static_cast<float>(display_diameter_px_) / 2.0f;
    const int32_t cx = coords.x1 + static_cast<int32_t>(std::lround(half));
    const int32_t cy = coords.y1 + static_cast<int32_t>(std::lround(half));

    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.center.x = cx;
    dsc.center.y = cy;
    // lv_draw_arc's `radius` is the OUTER edge and the stroke grows inward from it, so half the
    // width is added to put the stroke's centre-line on the design's r=114.5 rather than its
    // outside edge (measured: without this the ring sits ~2.5px small).
    dsc.radius = static_cast<int32_t>(std::lround((kRadiusRef + kStrokeRef / 2.0f) * scale));
    dsc.width = std::max<int32_t>(1, static_cast<int32_t>(std::lround(kStrokeRef * scale)));
    dsc.opa = LV_OPA_COVER;
    // Rounded ends, so the two ends either side of the ETA and every colour change read as a bar
    // rather than a set of sliced wedges.
    dsc.rounded = 1;

    // Maps a whole-trip per-mille onto the arc. The consumed part is dropped from the start, so the
    // remaining trip is re-spread across the FULL arc: the ring shortens toward the destination in
    // the sense that less of the route is left, while still using the whole ring to show it.
    // (Consuming it as a shrinking arc instead is a one-line change here if that reads better.)
    const float remaining = 1000.0f - static_cast<float>(progress_permille_);
    const auto to_deg = [&](float permille) {
        if (remaining <= 0.0f) return kArcEndDeg;
        const float t = (permille - static_cast<float>(progress_permille_)) / remaining;
        return kArcStartDeg + std::min(1.0f, std::max(0.0f, t)) * kArcSweepDeg;
    };

    if (count_ == 0) {
        // No traffic data is NOT "clear road" -- draw the whole remaining trip neutral grey.
        dsc.color = color_for(NAV_TRAFFIC_UNKNOWN);
        dsc.start_angle = kArcStartDeg;
        dsc.end_angle = kArcEndDeg;
        lv_draw_arc(layer, &dsc);
        return;
    }

    for (int i = 0; i < count_; ++i) {
        const nav_traffic_span_t &s = spans_[i];
        if (s.end_permille <= progress_permille_) continue; // already ridden
        const float a0 = to_deg(static_cast<float>(s.start_permille));
        const float a1 = to_deg(static_cast<float>(s.end_permille));
        if (a1 - a0 < 0.5f) continue; // sub-degree slivers just alias; not worth a draw call
        dsc.color = color_for(s.level);
        dsc.start_angle = a0;
        dsc.end_angle = a1;
        lv_draw_arc(layer, &dsc);
    }
}
