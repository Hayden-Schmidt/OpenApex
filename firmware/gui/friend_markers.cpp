#include "friend_markers.hpp"

#include <cmath>
#include <cstring>

#include "avatar_art.hpp"
#include "avatars_data.h"
#include "settings.h"

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Centre of a marker as a fraction of the ring radius: inside the major ticks (0.10, see
// compass_ring.cpp) with room for the marker's own half-height.
constexpr float kRadiusFrac = 0.80f;
// Character markers are drawn this many times their 1x art per 240px of panel.
constexpr float kCharScalePer240 = 2.0f;
constexpr float kDotDiameterPer240 = 10.0f;
constexpr float kDotOutlinePer240 = 2.0f;

struct Marker {
    int16_t bearing_deg;  // whole degrees, true
    uint8_t avatar;
    uint32_t accent;
    uint8_t buf[avatar_art_bytes(AVATAR_SMALL_W, AVATAR_SMALL_H)];
    lv_image_dsc_t img;
};

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
        changed = true;
    }
    return changed;
}

void friend_markers_draw(lv_layer_t *layer, float cx, float cy, float half, float heading_rad,
                         float scale) {
    if (!s_show) return;
    const float per240 = half / 120.0f;
    const float r = half * kRadiusFrac;
    for (int i = 0; i < s_count; ++i) {
        const Marker &m = s_markers[i];
        // Heading-up, as the north marker: screen angle is bearing minus heading, clockwise from up.
        const float a = static_cast<float>(m.bearing_deg) * kPi / 180.0f - heading_rad - kPi / 2.0f;
        const int32_t x = static_cast<int32_t>(std::lround(cx + std::cos(a) * r));
        const int32_t y = static_cast<int32_t>(std::lround(cy + std::sin(a) * r));

        if (s_dot) {
            const int32_t d = static_cast<int32_t>(std::lround(kDotDiameterPer240 * per240));
            const int32_t o = static_cast<int32_t>(std::lround(kDotOutlinePer240 * per240));
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
        dsc.pivot = {AVATAR_SMALL_W / 2, AVATAR_SMALL_H / 2};
        const float zoom = std::round(kCharScalePer240 * per240 / scale) * scale;
        dsc.scale_x = dsc.scale_y = static_cast<int32_t>(std::lround(zoom * LV_SCALE_NONE));
        const lv_area_t area = {x - AVATAR_SMALL_W / 2, y - AVATAR_SMALL_H / 2,
                                x - AVATAR_SMALL_W / 2 + AVATAR_SMALL_W - 1,
                                y - AVATAR_SMALL_H / 2 + AVATAR_SMALL_H - 1};
        lv_draw_image(layer, &dsc, &area);
    }
}
