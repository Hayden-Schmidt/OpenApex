#include "avatar_art.hpp"

#include "avatars_data.h"
#include "settings.h"

// lv_image_cache_drop: not re-exported by lvgl.h in v9.
#include "src/misc/cache/lv_cache.h"

namespace {

constexpr int kCustomPx = 16;
constexpr int kCustomSmallPx = 8;
constexpr int kCustomSmallOffset = 64;  // the 8x8 follows the 16x16 (avatar_store.h)

void put_px(uint8_t *buf, int w, int h, int x, int y, lv_color_t c, uint8_t alpha) {
    const int i = y * w + x;
    const uint16_t c16 = lv_color_to_u16(c);
    buf[i * 2] = c16 & 0xFF;
    buf[i * 2 + 1] = c16 >> 8;
    buf[w * h * 2 + i] = alpha;
}

// Main colour: the accent's hue at the preset's saturation and brightness (scaled by the accent's
// own). Other themed colours: the main colour turned by dh, saturation and brightness scaled by x/64.
lv_color_t resolve_palette(const avatar_preset_t &p, const avatar_pal_t &e, lv_color_hsv_t accent) {
    if (!e.themed) return lv_color_make(e.a, e.b, e.c);
    const int s = accent.s * p.main_s / 255 * e.b / 64;
    const int v = accent.v * p.main_v / 255 * e.c / 64;
    const int h = (accent.h + static_cast<int8_t>(e.a) + 360) % 360;
    return lv_color_hsv_to_rgb(h, s > 100 ? 100 : s, v > 100 ? 100 : v);
}

// 4-bit indexed art, `art_w` wide, drawn `scale`x into a w x h buffer at (ox, oy).
void draw_indexed(uint8_t *buf, int w, int h, const avatar_preset_t &p, const uint8_t *art, int art_w,
                  int art_h, int scale, lv_color_t accent) {
    const lv_color_hsv_t hsv = lv_color_to_hsv(accent);
    lv_color_t pal[AVATAR_PALETTE_MAX];
    for (int i = 0; i < p.palette_len; ++i) pal[i] = resolve_palette(p, p.palette[i], hsv);
    for (int y = 0; y < art_h * scale; ++y) {
        for (int x = 0; x < art_w * scale; ++x) {
            const int i = y / scale * art_w + x / scale;
            const int n = (art[i / 2] >> (i % 2 ? 0 : 4)) & 0xF;
            put_px(buf, w, h, x, y, n ? pal[n - 1] : lv_color_black(), n ? 255 : 0);
        }
    }
}

// 2-bit alpha art in the accent, `art_px` square, drawn `scale`x at (ox, oy).
void draw_a2(uint8_t *buf, int w, int h, const uint8_t *a2, int art_px, int scale, int ox, int oy,
             lv_color_t accent) {
    static const uint8_t kAlpha[4] = {0, 85, 170, 255};
    for (int y = 0; y < art_px * scale; ++y) {
        for (int x = 0; x < art_px * scale; ++x) {
            const int i = y / scale * art_px + x / scale;
            put_px(buf, w, h, ox + x, oy + y, accent, kAlpha[(a2[i / 4] >> (6 - 2 * (i % 4))) & 3]);
        }
    }
}

const avatar_preset_t &preset_for(uint8_t avatar) {
    if (avatar >= 1 && avatar <= AVATAR_PRESET_COUNT) return kAvatarPresets[avatar - 1];
    return kAvatarPresets[SETTINGS_AVATAR_DEFAULT_PRESET - 1];
}

} // namespace

void avatar_art_draw(uint8_t *buf, int px, uint8_t avatar, const uint8_t *custom, lv_color_t accent) {
    if (avatar == SETTINGS_AVATAR_CUSTOM && custom != nullptr) {
        draw_a2(buf, px, px, custom, kCustomPx, px / kCustomPx, 0, 0, accent);
        return;
    }
    const avatar_preset_t &p = preset_for(avatar);
    draw_indexed(buf, px, px, p, p.art, AVATAR_ART_PX, AVATAR_ART_PX, px / AVATAR_ART_PX, accent);
}

void avatar_art_draw_small(uint8_t *buf, uint8_t avatar, const uint8_t *custom, lv_color_t accent) {
    constexpr int w = AVATAR_SMALL_W, h = AVATAR_SMALL_H;
    if (avatar == SETTINGS_AVATAR_CUSTOM && custom != nullptr) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) put_px(buf, w, h, x, y, lv_color_black(), 0);
        }
        draw_a2(buf, w, h, custom + kCustomSmallOffset, kCustomSmallPx, 1, (w - kCustomSmallPx) / 2,
                (h - kCustomSmallPx) / 2, accent);
        return;
    }
    const avatar_preset_t &p = preset_for(avatar);
    draw_indexed(buf, w, h, p, p.small, w, h, 1, accent);
}

void avatar_art_describe(lv_image_dsc_t *dsc, const uint8_t *buf, int w, int h) {
    // Descriptors are reused, so drop LVGL's cached decode of the old pixels.
    if (dsc->data != nullptr) lv_image_cache_drop(dsc);
    *dsc = {};
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.w = static_cast<uint32_t>(w);
    dsc->header.h = static_cast<uint32_t>(h);
    dsc->header.stride = static_cast<uint32_t>(w * 2);
    dsc->data_size = static_cast<uint32_t>(avatar_art_bytes(w, h));
    dsc->data = buf;
}
