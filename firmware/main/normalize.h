#pragma once

#include "nav_model.h"
#include "packet.h"

// Normalizes a raw relay packet into the source-agnostic navigation model. This is the single
// place maneuver/distance/street text is interpreted on the terminal. The same function serves
// the Android relay path (title text carries the maneuver) and, later, the iOS/ANCS path (raw
// title/message text arrives directly). The input title is the raw navigation text; distance may
// arrive as a clean numeric field (Android shortCriticalText) or only embedded in text (ANCS) —
// both are handled here.
void normalize_packet(const raw_notif_t *raw, nav_model_t *out);

// ---------------------------------------------------------------------------------------------
// Glyph table introspection — for tests and tooling, not the terminal's runtime path.
//
// The maneuver-arrow "rotation angle" is a per-glyph constant, not geometry (see normalize.c), and
// it is matched with a small tolerance so a Maps release that nudges a bitmap by a degree does not
// silently produce UNKNOWN. That tolerance is only safe while no two entries in a table sit within
// 2x it of each other -- otherwise widening recognition would start turning one maneuver into
// another, which is exactly the class of bug the glyph table replaced. The tables are exposed so
// the host tests can prove that invariant holds as entries are added, instead of it living as a
// comment someone has to remember.
// ---------------------------------------------------------------------------------------------

#include <stddef.h>

typedef struct {
    int16_t angle_deg;
    nav_icon_t icon;
} glyph_entry_t;

const glyph_entry_t *normalize_maneuver_glyphs(size_t *count);
const glyph_entry_t *normalize_roundabout_glyphs(size_t *count);
int normalize_glyph_tolerance_deg(void);

// Shortest angular distance between two bearings, 0..180 (wrapping: 359 and 1 are 2 apart).
int normalize_angle_separation(int a, int b);
