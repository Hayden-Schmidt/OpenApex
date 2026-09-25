#pragma once

#include "lvgl.h"

// Picks a built-in Montserrat face by target pixel size.
//
// Pages author type as a glyph height measured off the Figma SVG, then ask for the nearest face at
// or below it -- the same "reference-design pixels scaled to the live panel" convention the rest of
// the geometry uses. Shared rather than per-page because the idle and odometer pages both need it
// and must not drift apart on which size they round to.
//
// Only the sizes enabled in lv_conf.h (sim) / sdkconfig (device) are candidates; each one costs
// flash, so they are turned on deliberately rather than wholesale.
const lv_font_t *montserrat_at_most(int32_t px);
