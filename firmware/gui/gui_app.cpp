#include "gui_app.hpp"

#include "basic_theme.hpp"
#include "board_profile.h"
#include "dial_screen.hpp"
#include "idle_screen.hpp"
#include "rich_theme.hpp"

namespace {

#if BOARD_GFX_TIER == BOARD_GFX_TIER_RICH
RichTheme s_theme;
#else
BasicTheme s_theme;
#endif

Screen *s_active = nullptr;

Screen *screen_for_state(view_state_t state) {
    static IdleScreen idle;
    static DialScreen dial;
    return state == VIEW_IDLE ? static_cast<Screen *>(&idle) : static_cast<Screen *>(&dial);
}

} // namespace

const GuiTheme &gui_theme(void) { return s_theme; }

void gui_app_init(void) {
    s_active = screen_for_state(VIEW_IDLE);
    s_theme.apply_state_change(nullptr, s_active);
}

void gui_app_update(const terminal_view_state_t *state) {
    Screen *next = screen_for_state(state->state);
    if (next != s_active) {
        s_theme.apply_state_change(s_active, next);
        s_active = next;
    }
    s_active->update(*state);
}
