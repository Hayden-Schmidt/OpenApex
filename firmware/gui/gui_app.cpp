#include "gui_app.hpp"

#include "arrived_screen.hpp"
#include "avatar_art.hpp"
#include "avatars_data.h"
#include "basic_theme.hpp"
#include "board_profile.h"
#include "call_screen.hpp"
#include "friend_markers.hpp"
#include "friend_screen.hpp"
#include "dial_screen.hpp"
#include "icons.hpp"
#include "idle_screen.hpp"
#include "odometer_screen.hpp"
#include "rich_theme.hpp"
#include "settings.h"

#include <cstring>

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

// Transition state ("design/SCREEN_TRANSITIONS.md"). While s_active plays its leave(),
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
// "design/screens/01-startup/NOTES.md": the logo must stay up for at least 2s while
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

// Rider settings (settings.h) as last applied here. Colour reaches most pages through the shared
// accent style; the dial ring is a start-up choice that touch-hold can still flip, so it is only
// pushed when the setting itself changes, never re-asserted over the rider's hold.
uint32_t s_applied_generation = UINT32_MAX;
uint8_t s_applied_dial_outer = GUI_DIAL_OUTER_COMPASS;

// The avatar on show, drawn at 1x (AVATAR_ART_PX) and scaled to BOARD_AVATAR_PX by the splash's
// lv_image with nearest-neighbour, which keeps pixel art identical to a pre-scaled copy. Drawing it
// pre-scaled cost 27 KB of static RAM on the C3 for a screen shown only at boot. A preset is full
// colour, its themed palette entries resolved against the accent (avatars_data.h); the rider's
// upload is 2-bit art in the accent alone. Both land in one RGB565A8 buffer: colour plane, then
// alpha plane.
static_assert((BOARD_AVATAR_PX / AVATAR_ART_PX) * AVATAR_ART_PX == BOARD_AVATAR_PX, "avatar must be a whole multiple");
uint8_t s_avatar_buf[avatar_art_bytes(AVATAR_ART_PX, AVATAR_ART_PX)];
lv_image_dsc_t s_avatar;
const void *s_avatar_shown = nullptr; // art currently drawn into s_avatar_buf
lv_color_t s_avatar_accent;           // and the accent it was drawn with
uint8_t s_custom[GUI_AVATAR_BYTES];
bool s_have_custom = false;

// The selected art: an avatar_preset_t or the 80-byte upload. The default character stands in for
// anything else, including "custom" with nothing uploaded.
const void *selected_avatar(void) {
    const uint8_t v = settings_get()->avatar;
    if (v == SETTINGS_AVATAR_CUSTOM && s_have_custom) return s_custom;
    if (v >= 1 && v <= AVATAR_PRESET_COUNT) return &kAvatarPresets[v - 1];
    return &kAvatarPresets[SETTINGS_AVATAR_DEFAULT_PRESET - 1];
}

gui_page_override_t s_page_override = GUI_PAGE_AUTO;
gui_nav_entry_t s_nav_entry = GUI_NAV_ENTRY_ELEMENTS;

IdleScreen &idle_screen(void) {
    static IdleScreen idle;
    return idle;
}

FriendScreen &friend_screen(void) {
    static FriendScreen friends;
    return friends;
}

CallScreen &call_screen(void) {
    static CallScreen call;
    return call;
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
// moment routing asks, because a late turn instruction is worse than a clipped animation. A ringing
// phone skips the hold too -- it only rings for so long.
bool may_leave(const Screen *next) {
    if (s_active != &idle_screen() || next == &call_screen() || next == &friend_screen()) return true;
    if (!s_active->entered()) return false;
    if (!s_entered_seen) {
        s_entered_seen = true;
        s_entered_ms = lv_tick_get();
    }
    return lv_tick_elaps(s_entered_ms) >= kIdleLeaveHoldMs;
}

// The splash is the only page showing the logo, and only for the first seconds after boot; a change
// arriving then (a phone that connected fast) swaps it in place.
void refresh_logo(void) {
#if BOARD_HAS_SPLASH
    if (!s_splash_done && s_active == &splash_screen()) splash_screen().set_logo(gui_logo_src());
#endif
}

void apply_settings(void) {
    const uint32_t gen = settings_generation();
    if (gen == s_applied_generation) return;
    s_applied_generation = gen;
    const terminal_settings_t *s = settings_get();

    const lv_color_t accent = s_theme.palette();
    lv_style_t *style = gui_accent_style();
    lv_style_set_text_color(style, accent);
    lv_style_set_bg_color(style, accent);
    lv_style_set_image_recolor(style, accent);
    lv_obj_report_style_change(style);

    if (s->dial_outer != s_applied_dial_outer) {
        s_applied_dial_outer = s->dial_outer;
        gui_app_set_dial_outer(static_cast<gui_dial_outer_t>(s->dial_outer));
    }
    refresh_logo();
}

} // namespace

lv_style_t *gui_accent_style(void) {
    static lv_style_t style;
    static bool init = false;
    if (!init) {
        lv_style_init(&style);
        init = true;
    }
    return &style;
}

const void *gui_logo_src(void) {
    const void *art = selected_avatar();
    const lv_color_t accent = s_theme.palette();
    if (art != s_avatar_shown || !lv_color_eq(accent, s_avatar_accent)) {
        avatar_art_draw(s_avatar_buf, AVATAR_ART_PX, settings_get()->avatar,
                        s_have_custom ? s_custom : nullptr, accent);
        avatar_art_describe(&s_avatar, s_avatar_buf, AVATAR_ART_PX, AVATAR_ART_PX);
        s_avatar_shown = art;
        s_avatar_accent = accent;
    }
    return &s_avatar;
}

const uint8_t *gui_app_custom_avatar(void) { return s_have_custom ? s_custom : nullptr; }

void gui_app_set_avatar(const uint8_t *a2) {
    s_have_custom = a2 != nullptr;
    if (s_have_custom) memcpy(s_custom, a2, sizeof(s_custom));
    if (s_avatar_shown == s_custom) s_avatar_shown = nullptr; // same buffer, new pixels
    if (s_active != nullptr) refresh_logo();
}

void gui_app_set_page_override(gui_page_override_t page) { s_page_override = page; }

void gui_app_set_dial_outer(gui_dial_outer_t outer) {
    // Routes through screen_for_state so the DialScreen instance is the same static one the page
    // routing hands out, rather than a second copy.
    static_cast<DialScreen *>(screen_for_state(VIEW_ACTIVE))->set_outer(outer);
}

void gui_app_set_nav_entry(gui_nav_entry_t entry) { s_nav_entry = entry; }

void gui_app_set_call_accept_handler(gui_call_accept_cb_t cb) { call_screen().set_accept_handler(cb); }

void gui_app_set_friend_accept_handler(gui_friend_accept_cb_t cb) { friend_screen().set_accept_handler(cb); }

gui_nav_entry_t gui_app_nav_entry(void) { return s_nav_entry; }

const GuiTheme &gui_theme(void) { return s_theme; }

void gui_app_init(void) {
    apply_settings();
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
    apply_settings();
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
    // A call, ringing or answered, outranks every page, including a live maneuver: the rider
    // chose to take calls on the terminal, and the nav page is back the moment it clears.
    // A friend prompt comes next: mesh_core only raises one while the gate says the rider can look.
    Screen *next = (state->call_ringing || state->call_active)
                       ? static_cast<Screen *>(&call_screen())
                   : state->friend_prompt != MESH_PROMPT_NONE ? static_cast<Screen *>(&friend_screen())
                                                               : screen_for_state(state->state);
#if BOARD_HAS_SPLASH
    // Boot always passes through idle, even when navigation is already running on the phone: the
    // phone-connect entry plays out and idle's hold floor applies before the nav page takes over.
    if (s_active == &splash_screen()) next = &idle_screen();
#endif
    if (s_leaving) {
        s_pending = next;
    } else if (next != s_active && may_leave(next)) {
        s_pending = next;
        s_leaving = true;
        // A page that enters over its predecessor (the arrived reveal) covers it as it stands:
        // playing the old page's exit first would leave nothing on screen to cover.
        if (next->enters_over()) {
            on_leave_done(nullptr);
            return;
        }
        s_active->leave(on_leave_done, nullptr);
    }
    if (friend_markers_update(*state)) lv_obj_invalidate(s_active->root());
    s_active->update(*state);
}
