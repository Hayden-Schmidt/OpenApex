#pragma once

#include "lvgl.h"

// Simulator-only guard against the one failure mode subsetted fonts introduce.
//
// firmware/gui's faces are subsetted (tools/build_font_subset.py, design/fonts/fonts_manifest.json)
// so each size carries only the characters its labels are expected to produce. A character that was
// left out does not fail the build, throw, or log -- it draws as an empty box, which is easy to miss
// on a 240px panel and has already slipped through once (the thousands separator in "3,500 km").
//
// This walks the live widget tree every frame and reports any label character its own font cannot
// render, with the offending string, so a `--page all` run surfaces the gap instead of your eyes.
// Each (font, codepoint) pair is reported once. The simulator is the right home for it: it costs a
// tree walk per frame and the device has no console to read it on.
void font_check_scan(lv_obj_t *root);
