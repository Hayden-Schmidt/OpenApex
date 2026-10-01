#include "friend_markers.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "avatar_art.hpp"
#include "avatars_data.h"
#include "settings.h"

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Markers sit on the tick ring, the art's box kRimGapPer240 inside the panel edge, under the north
// marker (compass_ring.cpp draws ticks, then these, then north).
constexpr float kRimGapPer240 = 0.0f;
// Character markers are drawn this many times their 1x art per 240px of panel.
constexpr float kCharScalePer240 = 4.0f;
constexpr float kDotDiameterPer240 = 10.0f;
constexpr float kDotOutlinePer240 = 2.0f;

struct Marker {
    int16_t bearing_deg;  // whole degrees, true
    uint8_t avatar;
    uint32_t accent;
    uint8_t buf[avatar_art_bytes(AVATAR_SMALL_W, AVATAR_SMALL_H)];
    lv_image_dsc_t img;
    // The character's visible pixels inside the 12x8 box, which narrow characters do not fill (they
    // sit to its left). Placement centres these, not the box.
    float vis_cx, vis_cy, vis_hw, vis_hh;
};

// Visible bounds from the alpha plane (RGB565A8: colour plane, then alpha).
void measure(Marker &m) {
    const uint8_t *alpha = m.buf + AVATAR_SMALL_W * AVATAR_SMALL_H * 2;
    int x0 = AVATAR_SMALL_W, y0 = AVATAR_SMALL_H, x1 = -1, y1 = -1;
    for (int y = 0; y < AVATAR_SMALL_H; ++y) {
        for (int x = 0; x < AVATAR_SMALL_W; ++x) {
            if (alpha[y * AVATAR_SMALL_W + x] == 0) continue;
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x);
            y1 = std::max(y1, y);
        }
    }
    if (x1 < 0) {
        x0 = y0 = 0;
        x1 = AVATAR_SMALL_W - 1;
        y1 = AVATAR_SMALL_H - 1;
    }
    m.vis_cx = (x0 + x1 + 1) / 2.0f;
    m.vis_cy = (y0 + y1 + 1) / 2.0f;
    m.vis_hw = (x1 - x0 + 1) / 2.0f;
    m.vis_hh = (y1 - y0 + 1) / 2.0f;
}

Marker s_markers[VIEW_FRIENDS_MAX];
int s_count = 0;
bool s_show = false;
bool s_dot = false;

bool same_look(const Marker &m, const mesh_look_t &l) {
    return m.avatar == l.avatar && m.accent == l.accent_rgb;
}

} // namespace

bool friend_markers_update(const terminal_view_state_t &state) {
    const terminal_settings_t *s = settings_get();
    const bool show = s->friends_on_compass != 0;
    const bool dot = s->friend_marker == SETTINGS_FRIEND_MARKER_DOT;
    bool changed = show != s_show || dot != s_dot || state.friend_count != s_count;
    s_show = show;
    s_dot = dot;
    s_count = state.friend_count;

    for (int i = 0; i < s_count; ++i) {
        Marker &m = s_markers[i];
        const mesh_friend_marker_t &f = state.friends[i];
        const int16_t bearing = static_cast<int16_t>((std::lround(f.bearing_deg) % 360 + 360) % 360);
        if (bearing != m.bearing_deg) {
            m.bearing_deg = bearing;
            changed = true;
        }
        if (same_look(m, f.look) && m.img.data != nullptr) continue;
        m.avatar = f.look.avatar;
        m.accent = f.look.accent_rgb;
        // Friends carry no custom art in the POC: an uploaded avatar draws as the default preset.
        avatar_art_draw_small(m.buf, m.avatar, nullptr, lv_color_hex(m.accent));
        avatar_art_describe(&m.img, m.buf, AVATAR_SMALL_W, AVATAR_SMALL_H);
        measure(m);
        changed = true;
    }
    return changed;
}

void friend_markers_draw(lv_layer_t *layer, float cx, float cy, float half, float heading_rad,
                         float scale) {
    if (!s_show) return;
    const float per240 = half / 120.0f;
    const float zoom = std::round(kCharScalePer240 * per240 / scale) * scale;
    const int32_t d = static_cast<int32_t>(std::lround(kDotDiameterPer240 * per240));
    const int32_t o = static_cast<int32_t>(std::lround(kDotOutlinePer240 * per240));
    for (int i = 0; i < s_count; ++i) {
        const Marker &m = s_markers[i];
        // Heading-up, as the north marker: screen angle is bearing minus heading, clockwise from up.
        const float a = static_cast<float>(m.bearing_deg) * kPi / 180.0f - heading_rad - kPi / 2.0f;
        const float c = std::cos(a), sn = std::sin(a);
        // One radius at every bearing, measured from the character's visible pixels, so every marker
        // covers the ticks the same way and its widest side just meets the rim.
        const float reach = s_dot ? d / 2.0f + o : std::max(m.vis_hw, m.vis_hh) * zoom;
        const float r = half - reach - kRimGapPer240 * per240;
        const int32_t x = static_cast<int32_t>(std::lround(cx + c * r));
        const int32_t y = static_cast<int32_t>(std::lround(cy + sn * r));

        if (s_dot) {
            lv_draw_rect_dsc_t dsc;
            lv_draw_rect_dsc_init(&dsc);
            dsc.radius = LV_RADIUS_CIRCLE;
            dsc.bg_color = lv_color_hex(m.accent);
            dsc.border_color = lv_color_black();
            dsc.border_width = o;
            const lv_area_t area = {x - d / 2 - o, y - d / 2 - o, x + d / 2 + o, y + d / 2 + o};
            lv_draw_rect(layer, &dsc, &area);
            continue;
        }

        // Nearest-neighbour upscale keeps the pixel art crisp; `scale` is the page's entry zoom.
        lv_draw_image_dsc_t dsc;
        lv_draw_image_dsc_init(&dsc);
        dsc.src = &m.img;
        dsc.antialias = false;
        // The visible centre lands on the point; the zoom pivots there too.
        const int32_t pvx = static_cast<int32_t>(std::lround(m.vis_cx));
        const int32_t pvy = static_cast<int32_t>(std::lround(m.vis_cy));
        dsc.pivot = {pvx, pvy};
        dsc.scale_x = dsc.scale_y = static_cast<int32_t>(std::lround(zoom * LV_SCALE_NONE));
        const lv_area_t area = {x - pvx, y - pvy, x - pvx + AVATAR_SMALL_W - 1, y - pvy + AVATAR_SMALL_H - 1};
        lv_draw_image(layer, &dsc, &area);
    }
}
