#pragma once

// Access to the offline-rasterized A8 icon set (design/icons/README.md). The per-profile pixel
// data lives in firmware/gui/generated/icons_<profile>.h, which is picked here by the board
// profile so GUI code never names a profile itself -- the generator already scaled every icon by
// (profile width / 240), so an ICON_* id means "the right size for this panel".
//
// Including this header costs nothing but the enum: the generated payload is guarded by
// ICON_DATA_IMPL and instantiated only by icons.cpp.
//
// These are alpha masks, not colour bitmaps. Draw them with lv_image and set the colour at draw
// time via lv_obj_set_style_image_recolor() + lv_obj_set_style_image_recolor_opa(..., LV_OPA_COVER).

#include "board_profile.h"

#if defined(BOARD_PROFILE_PROTOTYPE_C3_GC9A01)
#include "generated/icons_prototype_c3_gc9a01.h"
#elif defined(BOARD_PROFILE_S3_AMOLED_175)
#include "generated/icons_s3_amoled_175.h"
#else
#error "No icon set for the selected board profile -- add it to SCREEN_PROFILES in tools/build_icon_raster.py and re-run it."
#endif

// Returns the descriptor for `id`, or nullptr if `id` is out of range.
const lv_image_dsc_t *icon_image(icon_id_t id);

// Debug/logging name for `id`, or "?" if out of range.
const char *icon_name(icon_id_t id);
