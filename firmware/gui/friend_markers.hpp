#pragma once

#include "lvgl.h"
#include "view_state.h"

// Friends in range on the compass ring (docs/OpenApex_SPEC.md §5.7): each friend's small character,
// or a dot, in their accent, just inside the ticks at their bearing. Setting 0x07 hides them, 0x08
// picks the style. One set for every page with a compass, the same way the heading is shared.

// Takes the latest friends from the view. True when the markers on screen would change, so the
// caller invalidates the page; bearings move in whole degrees to keep that rare.
bool friend_markers_update(const terminal_view_state_t &state);

// Draws into a compass's layer. (cx, cy) is the ring centre and `half` its radius; heading as
// CompassRing::set_north. Only called with a known heading: a bearing is meaningless without it.
void friend_markers_draw(lv_layer_t *layer, float cx, float cy, float half, float heading_rad,
                         float scale);
