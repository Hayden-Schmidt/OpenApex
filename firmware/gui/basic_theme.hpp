#pragma once

#include "theme.hpp"

// Leanest tier (C3/BASIC, §16.6): flat palette, instant screen swaps, no transition animation.
class BasicTheme : public GuiTheme {
public:
    lv_color_t palette() const override;
    void apply_state_change(Screen *prev, Screen *next) override;
};
