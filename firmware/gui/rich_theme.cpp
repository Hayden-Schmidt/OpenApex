#include "rich_theme.hpp"

#include "screen.hpp"

lv_color_t RichTheme::palette() const {
    // Same placeholder as BasicTheme for now -- see basic_theme.cpp.
    return lv_color_hex(0x00C2FF);
}

void RichTheme::apply_state_change(Screen *prev, Screen *next) {
    (void)prev;
    lv_screen_load(next->root());
}
