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

// Add friend (gui_app.hpp). Open is the GUI's own state; the mesh hears about it through the
// menu callback. `auto` = opened by an incoming request rather than the rider.
constexpr uint32_t kMenuIdleMs = 60000;
gui_friend_menu_cb_t s_friend_menu_cb = nullptr;
bool s_menu_open = false;
bool s_menu_auto = false;
bool s_menu_allowed = false;
uint32_t s_menu_activity_ms = 0;
mesh_prompt_state_t s_seen_prompt = MESH_PROMPT_NONE;
uint8_t s_seen_prompt_id[MESH_RIDER_ID_LEN];
bool s_input_hooked = false;
volatile bool s_menu_request = false; // gui_app_open_friend_menu, from any task

// A finger held still this long on any page but a call or Add friend itself opens Add friend.
constexpr uint32_t kMenuHoldMs = 5000;
constexpr int32_t kMenuHoldSlopPx = 20;
lv_indev_t *s_hold_indev = nullptr;
bool s_hold_armed = false;
uint32_t s_hold_ms = 0;
lv_point_t s_hold_at = {0, 0};

gui_page_override_t s_page_override = GUI_PAGE_AUTO;
// Why the odometer page is up: the rider's double tap over navigation (it stays until a turn is
// near), or speed (it goes back to idle after kAutoOdoStopMs stopped).
bool s_odo_over_nav = false;
bool s_odo_auto = false;
constexpr uint16_t kAutoOdoKmhX10 = 50;      // "riding": over 5 km/h
constexpr uint32_t kAutoOdoStopMs = 180000;  // stopped this long and the speed-raised page goes
bool s_was_riding = false;
bool s_stopped = false;
uint32_t s_stopped_ms = 0;
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

DialScreen &dial_screen(void) {
    static DialScreen dial;
    return dial;
}

OdometerScreen &odometer_screen(void) {
    static OdometerScreen odometer;
    return odometer;
}

void drop_odometer(void) {
    s_page_override = GUI_PAGE_AUTO;
    s_odo_over_nav = false;
    s_odo_auto = false;
}

// Within this of the next maneuver the nav page takes the screen back from anything the rider chose.
bool turn_near(const terminal_view_state_t &st) {
    return st.state == VIEW_ACTIVE && st.distance_meters <= MESH_PROMPT_MIN_TURN_M;
}

Screen *screen_for_state(const terminal_view_state_t &st) {
    static ArrivedScreen arrived;
    const view_state_t state = st.state;

    // The odometer page outranks idle. Over navigation only when the rider asked for it there (double
    // tap on the nav page), and never near a turn: the nav page takes the screen back and the choice
    // is dropped. Navigation starting under it drops it too.
    if (s_page_override == GUI_PAGE_ODOMETER) {
        if (state == VIEW_IDLE) {
            s_odo_over_nav = false;
            return &odometer_screen();
        }
        if (state != VIEW_ARRIVED && s_odo_over_nav && !turn_near(st)) return &odometer_screen();
        drop_odometer();
    }
    if (state == VIEW_ARRIVED) return &arrived;
    return state == VIEW_IDLE ? static_cast<Screen *>(&idle_screen())
                              : static_cast<Screen *>(&dial_screen());
}

// Setting 0x0A: riding off navigation brings the odometer page up; three minutes stopped puts idle
// back, but only if speed is what raised it. Rising edge only, so a rider who double taps away from
// it while moving keeps their choice until the next stop.
void update_auto_odometer(const terminal_view_state_t &st) {
    const bool known = st.phone_connected && st.speed_kmh_x10 != NAV_U16_UNKNOWN;
    const bool riding = known && st.speed_kmh_x10 > kAutoOdoKmhX10;
    if (riding && !s_was_riding && settings_get()->auto_odometer != 0 && st.state == VIEW_IDLE &&
        s_page_override == GUI_PAGE_AUTO) {
        s_page_override = GUI_PAGE_ODOMETER;
        s_odo_auto = true;
    }
    if (known) s_was_riding = riding;
    if (!s_odo_auto || s_page_override != GUI_PAGE_ODOMETER) {
        s_odo_auto = false;
        s_stopped = false;
        return;
    }
    // No speed (phone gone) counts as stopped: a parked bike with the phone in the house goes idle.
    if (riding) {
        s_stopped = false;
    } else if (!s_stopped) {
        s_stopped = true;
        s_stopped_ms = lv_tick_get();
    } else if (lv_tick_elaps(s_stopped_ms) >= kAutoOdoStopMs) {
        drop_odometer();
    }
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
    if (s_active != &idle_screen() || next == &call_screen() || next == &friend_screen() ||
        next == &odometer_screen()) {
        return true;
    }
    if (!s_active->entered()) return false;
    if (!s_entered_seen) {
        s_entered_seen = true;
        s_entered_ms = lv_tick_get();
    }
    return lv_tick_elaps(s_entered_ms) >= kIdleLeaveHoldMs;
}

void set_friend_menu(bool open, bool by_request) {
    if (open == s_menu_open) return;
    s_menu_open = open;
    s_menu_auto = open && by_request;
    s_menu_activity_ms = lv_tick_get();
    if (s_friend_menu_cb != nullptr) s_friend_menu_cb(open);
}

// Double tap (U-table, owner 2026-10-01): on the idle or nav page it brings up the odometer page, on
// the odometer page it goes back; in Add friend it closes the menu, except on the card's edges,
// where taps step between riders. Watched on the input device rather than on each page, so pages
// full of child objects need nothing of their own.
void on_double_tap(lv_event_t *e) {
    auto *indev = static_cast<lv_indev_t *>(lv_event_get_current_target(e));
    if (lv_indev_get_short_click_streak(indev) != 2) return;
#if BOARD_HAS_SPLASH
    if (!s_splash_done) return;
#endif
    if (s_leaving) return;
    if (s_active == &friend_screen()) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        if (!friend_screen().in_step_zone(p.x)) set_friend_menu(false, false);
        return;
    }
    if (s_active == &odometer_screen()) {
        drop_odometer();
    } else if (s_active == &idle_screen()) {
        s_page_override = GUI_PAGE_ODOMETER;
        s_odo_auto = false;
    } else if (s_active == &dial_screen()) {
        s_page_override = GUI_PAGE_ODOMETER;
        s_odo_auto = false;
        s_odo_over_nav = true;
    }
}

// The 5 s hold that opens Add friend (U1). Armed on a press on any page but a call or the menu.
void on_press(lv_event_t *e) {
    s_hold_indev = static_cast<lv_indev_t *>(lv_event_get_current_target(e));
    s_hold_armed = s_active != &call_screen() && s_active != &friend_screen();
#if BOARD_HAS_SPLASH
    if (!s_splash_done) s_hold_armed = false;
#endif
    s_hold_ms = lv_tick_get();
    lv_indev_get_point(s_hold_indev, &s_hold_at);
}

void on_release(lv_event_t *e) {
    (void)e;
    s_hold_armed = false;
}

void hook_input(void) {
    if (s_input_hooked) return;
    s_input_hooked = true;
    for (lv_indev_t *i = lv_indev_get_next(nullptr); i != nullptr; i = lv_indev_get_next(i)) {
        lv_indev_add_event_cb(i, on_double_tap, LV_EVENT_SHORT_CLICKED, nullptr);
        lv_indev_add_event_cb(i, on_press, LV_EVENT_PRESSED, nullptr);
        lv_indev_add_event_cb(i, on_release, LV_EVENT_RELEASED, nullptr);
    }
}

// The finger has to stay put for the whole hold. When it opens the menu the press is cancelled and
// the input device ignores the finger until it lifts: the page under it gets no release (the nav
// page's ring swap stays unfired) and Add friend sees no press, so the same hold cannot run on
// into the friend ring.
void check_menu_hold(void) {
    if (!s_hold_armed || s_hold_indev == nullptr) return;
    lv_point_t p;
    lv_indev_get_point(s_hold_indev, &p);
    const int32_t dx = p.x - s_hold_at.x, dy = p.y - s_hold_at.y;
    if (dx * dx + dy * dy > kMenuHoldSlopPx * kMenuHoldSlopPx || s_active == &call_screen()) {
        s_hold_armed = false;
        return;
    }
    if (lv_tick_elaps(s_hold_ms) < kMenuHoldMs) return;
    s_hold_armed = false;
    if (!s_menu_allowed) return;
    lv_indev_reset(s_hold_indev, nullptr);
    lv_indev_wait_release(s_hold_indev);
    set_friend_menu(true, false);
}

// Opens and closes Add friend on the mesh's prompt edges and the gate.
void update_friend_menu(const terminal_view_state_t &st) {
    hook_input();
    s_menu_allowed = st.friend_menu_allowed;
    if (s_menu_request) {
        s_menu_request = false;
        if (s_menu_allowed) set_friend_menu(true, false);
    }
    const mesh_prompt_state_t was = s_seen_prompt;
    const bool changed = st.friend_prompt != s_seen_prompt ||
                         std::memcmp(st.friend_prompt_id, s_seen_prompt_id, MESH_RIDER_ID_LEN) != 0;
    if (changed) {
        s_seen_prompt = st.friend_prompt;
        std::memcpy(s_seen_prompt_id, st.friend_prompt_id, MESH_RIDER_ID_LEN);
        s_menu_activity_ms = lv_tick_get();
    }
    if (!st.friend_menu_allowed) {
        set_friend_menu(false, false); // a call, or a turn coming up
        return;
    }
    // Someone asked us (always discoverable), or a handshake surfaced again after a turn.
    if (changed && st.friend_prompt != MESH_PROMPT_NONE && !s_menu_open) set_friend_menu(true, true);
    if (!s_menu_open) return;
    if (changed && st.friend_prompt == MESH_PROMPT_NONE) {
        // "Added!" has had its 3 s: back to whatever was up before.
        if (was == MESH_PROMPT_DONE) {
            set_friend_menu(false, false);
            return;
        }
        // A request that opened the menu by itself went unanswered.
        if (was == MESH_PROMPT_OFFER && s_menu_auto) {
            set_friend_menu(false, false);
            return;
        }
    }
    if (st.friend_prompt == MESH_PROMPT_NONE && lv_tick_elaps(s_menu_activity_ms) >= kMenuIdleMs &&
        lv_tick_elaps(friend_screen().last_touch_ms()) >= kMenuIdleMs) {
        set_friend_menu(false, false);
    }
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
    // The same DialScreen instance the page routing hands out.
    dial_screen().set_outer(outer);
}

void gui_app_set_nav_entry(gui_nav_entry_t entry) { s_nav_entry = entry; }

void gui_app_set_call_accept_handler(gui_call_accept_cb_t cb) { call_screen().set_accept_handler(cb); }

void gui_app_set_friend_handlers(gui_friend_hold_cb_t hold, gui_friend_menu_cb_t menu) {
    friend_screen().set_hold_handler(hold);
    s_friend_menu_cb = menu;
}

void gui_app_open_friend_menu(void) { s_menu_request = true; }

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
    // Add friend comes next: it is only open while the gate says the rider can look away from the
    // road. Closing it falls back to whatever the state routes to, so the rider lands back where
    // they were (or on the nav page, if navigation started meanwhile).
    update_friend_menu(*state);
    check_menu_hold();
    update_auto_odometer(*state);
    Screen *next = (state->call_ringing || state->call_active) ? static_cast<Screen *>(&call_screen())
                   : s_menu_open                               ? static_cast<Screen *>(&friend_screen())
                                                               : screen_for_state(*state);
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
