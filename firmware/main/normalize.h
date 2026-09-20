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
