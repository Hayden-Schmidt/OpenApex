#include "gui_font.hpp"

#include "generated/fonts.h"

// The faces themselves live in firmware/gui/generated/font_montserrat_<size>.c, built by
// tools/build_font_subset.py from design/fonts/fonts_manifest.json.
//
// They are C files and have to stay C files: lv_font_conv writes the lv_font_t initializer with its
// designators in the order LVGL's own built-in fonts use, which is not lv_font_t's declaration
// order -- fine in C, an error in C++. That also keeps lv_font_conv's identical file-static names
// (glyph_bitmap, font_dsc, cmaps...) in separate translation units, which is why LVGL ships fonts
// this way too.
//
// To add or remove a size: edit the manifest, run `python tools/build_font_subset.py`, then add the
// file to firmware/gui/CMakeLists.txt and the entry to kFaces below. No sdkconfig/lv_conf font
// option is involved any more except for size 14.

namespace {

struct Face {
    int32_t px;
    const lv_font_t *font;
};

// Ascending by size. This is every face we ship, and therefore also the exact flash cost of text --
// unlike the old built-in lookup, there is nothing here the linker might have dropped.
constexpr Face kFaces[] = {
    // 14 is the one face we do not subset: it draws the street name and ETA, which arrive from the
    // phone as arbitrary text. LVGL's built-in is 8.6 KB and is already LV_FONT_DEFAULT, so
    // subsetting it would only mean shipping two 14px faces.
    {14, &lv_font_montserrat_14},
    {18, &gui_font_montserrat_18},
    {20, &gui_font_montserrat_20},
    {28, &gui_font_montserrat_28},
    {38, &gui_font_montserrat_38},
    {44, &gui_font_montserrat_44},
};

} // namespace

const lv_font_t *montserrat_at_most(int32_t px) {
    // Largest face that still fits. Below the smallest, return the smallest rather than
    // LV_FONT_DEFAULT: a caller asking for 10px wants small text, and on a scaled-down panel
    // LV_FONT_DEFAULT is not guaranteed to be one of the faces we ship.
    const lv_font_t *best = kFaces[0].font;
    for (const Face &f : kFaces) {
        if (f.px <= px) best = f.font;
    }
    return best;
}
