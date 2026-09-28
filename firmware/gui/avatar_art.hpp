#pragma once

#include <cstdint>

#include "lvgl.h"

// Rider characters as LVGL images: a preset (avatars_data.h), full colour with its themed palette
// entries resolved against an accent, or uploaded 2-bit art (avatar_store.h) in the accent alone.
// Shared by the boot logo (gui_app.cpp), the friend prompt and the compass friend markers, so every
// rider -- ours or a friend's -- is drawn by the same rules.
//
// Buffers are RGB565A8: colour plane (w*h*2) then alpha plane (w*h), avatar_art_bytes(w, h) long.

constexpr int avatar_art_bytes(int w, int h) { return w * h * 3; }

// Full art: a preset's 32x32 or the upload's 16x16, at `px` (a whole multiple of both). `custom`
// is the 80-byte upload, or null for none; `avatar` is a settings avatar value, and an upload
// asked for but missing falls back to the default character.
void avatar_art_draw(uint8_t *buf, int px, uint8_t avatar, const uint8_t *custom, lv_color_t accent);

// Marker art at 1x, AVATAR_SMALL_W x AVATAR_SMALL_H: a preset's small art, or the upload's 8x8
// centred in it.
void avatar_art_draw_small(uint8_t *buf, uint8_t avatar, const uint8_t *custom, lv_color_t accent);

// Points `dsc` at `buf`, and drops LVGL's cached decode of whatever it pointed at before.
void avatar_art_describe(lv_image_dsc_t *dsc, const uint8_t *buf, int w, int h);
