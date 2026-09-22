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

    // Switches the active screen from `prev` (nullptr on first load) to `next`.
    virtual void apply_state_change(Screen *prev, Screen *next) = 0;
};
