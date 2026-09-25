#include "gui_app.hpp"

#include "basic_theme.hpp"
#include "board_profile.h"
#include "dial_screen.hpp"
#include "idle_screen.hpp"
#include "rich_theme.hpp"

#if BOARD_HAS_SPLASH
#include "splash_screen.hpp"
#endif

namespace {

#if BOARD_GFX_TIER == BOARD_GFX_TIER_RICH
RichTheme s_theme;
#else
BasicTheme s_theme;
#endif

Screen *s_active = nullptr;

#if BOARD_HAS_SPLASH
// "design reference/1. Startup Screen/bootscreen.md": the logo must stay up for at least 2s while
// the rest of the terminal starts. The hold lives here rather than in SplashScreen because it is a
// routing decision -- the splash is not a view_state_t, it is what we show *before* honouring one.
// It is a floor, not a delay: by the time gui_task runs, BLE and NVS are already up (main.c), so on
// a fast boot this is the only thing keeping the logo visible at all.
constexpr uint32_t kSplashMinMs = 2000;
uint32_t s_boot_ms = 0;
bool s_splash_done = false;

SplashScreen &splash_screen(void) {
    static SplashScreen splash;
    return splash;
}
#endif

Screen *screen_for_state(view_state_t state) {
    static IdleScreen idle;
    static DialScreen dial;
    return state == VIEW_IDLE ? static_cast<Screen *>(&idle) : static_cast<Screen *>(&dial);
}

} // namespace

const GuiTheme &gui_theme(void) { return s_theme; }

void gui_app_init(void) {
#if BOARD_HAS_SPLASH
    s_boot_ms = lv_tick_get();
    s_splash_done = false;
    s_active = &splash_screen();
#else
    s_active = screen_for_state(VIEW_IDLE);
#endif
    s_theme.apply_state_change(nullptr, s_active);
}

void gui_app_update(const terminal_view_state_t *state) {
#if BOARD_HAS_SPLASH
    if (!s_splash_done) {
        if (lv_tick_elaps(s_boot_ms) < kSplashMinMs) {
            s_active->update(*state);
            return;
        }
        s_splash_done = true;
    }
#endif
    Screen *next = screen_for_state(state->state);
    if (next != s_active) {
        s_theme.apply_state_change(s_active, next);
        s_active = next;
    }
    s_active->update(*state);
}
