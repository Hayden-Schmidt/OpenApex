#pragma once

#include "lvgl.h"

class Screen;

// Governs Layer 1's *how*, never its *what* (§16.6 GUI architecture): palette and state-change
// transition style. BasicTheme/RichTheme are selected at compile time by BOARD_GFX_TIER. Further
// hooks (e.g. icon fidelity) are added once screen content needs them, not ahead of time.
class GuiTheme {
public:
    virtual ~GuiTheme() = default;

    virtual lv_color_t palette() const = 0;

    // Semantic status colour for a completed navigation (the arrived pop-up's surface). Separate
    // from palette() because it means "success", not "brand" -- it must stay green when the rider
    // themes the device purple. Lives on the theme rather than as a literal in ArrivedScreen so a
    // tier or a future runtime config can move it.
    virtual lv_color_t arrived_surface() const = 0;

    // Switches the active screen from `prev` (nullptr on first load) to `next` and starts its
    // entry. `prev` has already played its leave() by the time this runs.
    virtual void apply_state_change(Screen *prev, Screen *next) = 0;
};
