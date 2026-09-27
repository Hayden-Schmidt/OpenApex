#include "rich_theme.hpp"

#include "screen.hpp"
#include "settings.h"

lv_color_t RichTheme::palette() const {
    // The rider's colour, set from the phone and persisted (settings.h); defaults to the #00C2FF
    // placeholder until one is chosen. Read live so every caller follows a change.
    return lv_color_hex(settings_get()->accent_rgb);
}

// #1E8D3E, straight off "design reference/3.Turn by Turn/Arrived Pop Up.svg".
lv_color_t RichTheme::arrived_surface() const { return lv_color_hex(0x1E8D3E); }

void RichTheme::apply_state_change(Screen *prev, Screen *next) {
    // A page that reveals itself over the previous one (ArrivedScreen) loads its own root when
    // the reveal lands; every other page is loaded here and plays its own entry.
    if (prev == nullptr || !next->enters_over()) lv_screen_load(next->root());
    next->enter();
}
