#include "rich_theme.hpp"

#include "screen.hpp"

lv_color_t RichTheme::palette() const {
    // Same placeholder as BasicTheme for now -- see basic_theme.cpp.
    return lv_color_hex(0x00C2FF);
}

// #1E8D3E, straight off "design reference/3.Turn by Turn/Arrived Pop Up.svg".
lv_color_t RichTheme::arrived_surface() const { return lv_color_hex(0x1E8D3E); }

void RichTheme::apply_state_change(Screen *prev, Screen *next) {
    // A page that reveals itself over the previous one (ArrivedScreen) loads its own root when
    // the reveal lands; every other page is loaded here and plays its own entry.
    if (prev == nullptr || !next->enters_over()) lv_screen_load(next->root());
    next->enter();
}
