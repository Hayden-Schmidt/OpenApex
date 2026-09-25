#include "gui_app.hpp"

#include "arrived_screen.hpp"
#include "basic_theme.hpp"
#include "board_profile.h"
#include "dial_screen.hpp"
#include "idle_screen.hpp"
#include "odometer_screen.hpp"
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

// Transition state ("design reference/Screen Transitions.md"). While s_active plays its leave(),
// s_pending is where routing wants to go; the leave callback loads it. A target change mid-leave
// just retargets s_pending -- the page already on its way out does not restart.
Screen *s_pending = nullptr;
bool s_leaving = false;

// The idle page, fully landed, stays up at least this long before navigation takes it away -- a
// floor, not a delay: the clock starts when its entry finishes, not when navigation began.
constexpr uint32_t kIdleLeaveHoldMs = 2000;
uint32_t s_entered_ms = 0;
bool s_entered_seen = false;

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

gui_page_override_t s_page_override = GUI_PAGE_AUTO;
gui_nav_entry_t s_nav_entry = GUI_NAV_ENTRY_ELEMENTS;

IdleScreen &idle_screen(void) {
    static IdleScreen idle;
    return idle;
}

Screen *screen_for_state(view_state_t state) {
    static DialScreen dial;
    static OdometerScreen odometer;
    static ArrivedScreen arrived;

    // A rider-selected page outranks the idle state, but never an active maneuver: being shown the
    // odometer instead of the turn you are about to miss would be actively dangerous. Navigation
    // takes the screen back and the selection is dropped.
    if (s_page_override == GUI_PAGE_ODOMETER) {
        if (state == VIEW_IDLE) return &odometer;
        s_page_override = GUI_PAGE_AUTO;
    }
    if (state == VIEW_ARRIVED) return &arrived;
    return state == VIEW_IDLE ? static_cast<Screen *>(&idle_screen())
                              : static_cast<Screen *>(&dial);
}

void on_leave_done(void *ctx) {
    (void)ctx;
    Screen *prev = s_active;
    s_active = s_pending;
    s_leaving = false;
    s_entered_seen = false;
    s_theme.apply_state_change(prev, s_active);
}

// Whether s_active may start leaving now. Only the idle page holds: every other page gives way the
// moment routing asks, because a late turn instruction is worse than a clipped animation.
bool may_leave(void) {
    if (s_active != &idle_screen()) return true;
    if (!s_active->entered()) return false;
    if (!s_entered_seen) {
        s_entered_seen = true;
        s_entered_ms = lv_tick_get();
    }
    return lv_tick_elaps(s_entered_ms) >= kIdleLeaveHoldMs;
}

} // namespace

void gui_app_set_page_override(gui_page_override_t page) { s_page_override = page; }

void gui_app_set_dial_outer(gui_dial_outer_t outer) {
    // Routes through screen_for_state so the DialScreen instance is the same static one the page
    // routing hands out, rather than a second copy.
    static_cast<DialScreen *>(screen_for_state(VIEW_ACTIVE))->set_outer(outer);
}

void gui_app_set_nav_entry(gui_nav_entry_t entry) { s_nav_entry = entry; }

gui_nav_entry_t gui_app_nav_entry(void) { return s_nav_entry; }

const GuiTheme &gui_theme(void) { return s_theme; }

void gui_app_init(void) {
#if BOARD_HAS_SPLASH
    s_boot_ms = lv_tick_get();
    s_splash_done = false;
    s_active = &splash_screen();
#else
    s_active = screen_for_state(VIEW_IDLE);
#endif
    s_leaving = false;
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
    // Link status and every other live value keep flowing into the page while it animates: update()
    // runs every frame, leaving or not, so nothing is ever frozen behind a tween.
    Screen *next = screen_for_state(state->state);
#if BOARD_HAS_SPLASH
    // Boot always passes through idle, even when navigation is already running on the phone: the
    // phone-connect entry plays out and idle's hold floor applies before the nav page takes over.
    if (s_active == &splash_screen()) next = &idle_screen();
#endif
    if (s_leaving) {
        s_pending = next;
    } else if (next != s_active && may_leave()) {
        s_pending = next;
        s_leaving = true;
        s_active->leave(on_leave_done, nullptr);
    }
    s_active->update(*state);
}
