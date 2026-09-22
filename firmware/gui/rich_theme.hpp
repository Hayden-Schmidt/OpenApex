#pragma once

#include "theme.hpp"

// Richest tier (S3/RICH, §16.6). Starts visually identical to BasicTheme -- animated transitions
// (lv_screen_load_anim) are an additive follow-up once base screen content exists on both tiers.
class RichTheme : public GuiTheme {
public:
    lv_color_t palette() const override;
    void apply_state_change(Screen *prev, Screen *next) override;
};
