#include "font_check.hpp"

#include <cstdio>
#include <cstddef>
#include <set>
#include <utility>

namespace {

// Already-reported (font, codepoint) pairs, so a missing glyph on a label that redraws every frame
// produces one line rather than a scrolling wall.
std::set<std::pair<const lv_font_t *, uint32_t>> g_reported;

// Minimal UTF-8 decode. LVGL's own iterator lives in lv_text_private.h, which the simulator has no
// business reaching into, and the GUI's text is ASCII plus the odd Latin-1 sign anyway.
uint32_t next_codepoint(const char *s, size_t &i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    int extra = 0;
    uint32_t cp = c;
    if (c < 0x80) {
        extra = 0;
    } else if ((c & 0xE0) == 0xC0) {
        extra = 1;
        cp = c & 0x1Fu;
    } else if ((c & 0xF0) == 0xE0) {
        extra = 2;
        cp = c & 0x0Fu;
    } else if ((c & 0xF8) == 0xF0) {
        extra = 3;
        cp = c & 0x07u;
    } else {
        ++i; // invalid lead byte: skip it rather than looping forever
        return 0;
    }
    ++i;
    for (int k = 0; k < extra; ++k) {
        const unsigned char cc = static_cast<unsigned char>(s[i]);
        if ((cc & 0xC0) != 0x80) return 0; // truncated
        cp = (cp << 6) | (cc & 0x3Fu);
        ++i;
    }
    return cp;
}

bool renderable(const lv_font_t *font, uint32_t cp) {
    lv_font_glyph_dsc_t dsc;
    // letter_next = 0: we only care whether the glyph exists, not about kerning.
    return lv_font_get_glyph_dsc(font, &dsc, cp, 0);
}

void check_label(lv_obj_t *obj) {
    const char *text = lv_label_get_text(obj);
    if (text == nullptr) return;
    const lv_font_t *font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
    if (font == nullptr) return;

    size_t i = 0;
    while (text[i] != '\0') {
        const uint32_t cp = next_codepoint(text, i);
        if (cp == 0) continue; // malformed byte, already stepped over
        if (cp == ' ' || cp == '\n' || cp == '\r' || cp == '\t') continue; // no glyph to miss
        if (renderable(font, cp)) continue;
        if (!g_reported.insert({font, cp}).second) continue;
        std::printf("font_check: U+%04X is missing from the font used for \"%s\"\n"
                    "            -> add it to that size in design/fonts/fonts_manifest.json and "
                    "rerun tools/build_font_subset.py\n",
                    cp, text);
        std::fflush(stdout);
    }
}

void walk(lv_obj_t *obj) {
    if (obj == nullptr) return;
    // lv_obj_check_type rather than a cast: screens hold images, arcs and plain objects too.
    if (lv_obj_check_type(obj, &lv_label_class)) check_label(obj);
    const uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; ++i) walk(lv_obj_get_child(obj, i));
}

} // namespace

void font_check_scan(lv_obj_t *root) { walk(root); }
