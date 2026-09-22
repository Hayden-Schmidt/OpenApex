#include "basic_theme.hpp"

#include "screen.hpp"

lv_color_t BasicTheme::palette() const {
    // Placeholder high-contrast colour -- final brand colour TBC (design reference/DESIGN
    // NOTES.md), deliberately not yellow.
    return lv_color_hex(0x00C2FF);
}

void BasicTheme::apply_state_change(Screen *prev, Screen *next) {
    (void)prev;
    lv_screen_load(next->root());
}
