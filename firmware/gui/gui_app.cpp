#include "gui_app.hpp"

#include "arrived_screen.hpp"
#include "avatars_data.h"
#include "basic_theme.hpp"
#include "board_profile.h"
#include "call_screen.hpp"
#include "dial_screen.hpp"
#include "icons.hpp"
#include "idle_screen.hpp"
#include "odometer_screen.hpp"
#include "rich_theme.hpp"
#include "settings.h"

// lv_image_cache_drop: not re-exported by lvgl.h in v9.
#include "src/misc/cache/lv_cache.h"

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

// Rider settings (settings.h) as last applied here. Colour reaches most pages through the shared
// accent style; the dial ring is a start-up choice that touch-hold can still flip, so it is only
// pushed when the setting itself changes, never re-asserted over the rider's hold.
uint32_t s_applied_generation = UINT32_MAX;
uint8_t s_applied_dial_outer = GUI_DIAL_OUTER_COMPASS;

// The avatar on show, drawn at an integer scale so the pixels stay crisp. A preset is full colour,
// its themed palette entries resolved against the accent (avatars_data.h); the rider's upload is
// 2-bit art in the accent alone. Both land in one RGB565A8 buffer: colour plane, then alpha plane.
constexpr int kAvatarScale = BOARD_AVATAR_PX / AVATAR_ART_PX;
static_assert(kAvatarScale * AVATAR_ART_PX == BOARD_AVATAR_PX, "avatar must be a whole multiple");
constexpr int kCustomArtPx = 16;
constexpr int kCustomScale = BOARD_AVATAR_PX / kCustomArtPx;
constexpr int kAvatarPixels = BOARD_AVATAR_PX * BOARD_AVATAR_PX;
uint8_t s_avatar_buf[kAvatarPixels * 3];
lv_image_dsc_t s_avatar;
const void *s_avatar_shown = nullptr; // art currently drawn into s_avatar_buf
lv_color_t s_avatar_accent;           // and the accent it was drawn with
uint8_t s_custom[GUI_AVATAR_BYTES];
bool s_have_custom = false;

void put_avatar_px(int x, int y, lv_color_t c, uint8_t alpha) {
    const int i = y * BOARD_AVATAR_PX + x;
    const uint16_t c16 = lv_color_to_u16(c);
    s_avatar_buf[i * 2] = c16 & 0xFF;
    s_avatar_buf[i * 2 + 1] = c16 >> 8;
    s_avatar_buf[kAvatarPixels * 2 + i] = alpha;
}

// Main colour: the accent's hue at the preset's saturation and brightness (scaled by the accent's
// own). Other themed colours: the main colour turned by dh, saturation and brightness scaled by x/64.
lv_color_t resolve_palette(const avatar_preset_t &p, const avatar_pal_t &e, lv_color_hsv_t accent) {
    if (!e.themed) return lv_color_make(e.a, e.b, e.c);
    const int s = accent.s * p.main_s / 255 * e.b / 64;
    const int v = accent.v * p.main_v / 255 * e.c / 64;
    const int h = (accent.h + static_cast<int8_t>(e.a) + 360) % 360;
    return lv_color_hsv_to_rgb(h, s > 100 ? 100 : s, v > 100 ? 100 : v);
}

void draw_preset(const avatar_preset_t &p, lv_color_t accent) {
    const lv_color_hsv_t hsv = lv_color_to_hsv(accent);
    lv_color_t pal[AVATAR_PALETTE_MAX];
    for (int i = 0; i < p.palette_len; ++i) pal[i] = resolve_palette(p, p.palette[i], hsv);
    for (int y = 0; y < BOARD_AVATAR_PX; ++y) {
        for (int x = 0; x < BOARD_AVATAR_PX; ++x) {
            const int i = y / kAvatarScale * AVATAR_ART_PX + x / kAvatarScale;
            const int n = (p.art[i / 2] >> (i % 2 ? 0 : 4)) & 0xF;
            put_avatar_px(x, y, n ? pal[n - 1] : lv_color_black(), n ? 255 : 0);
        }
    }
}

void draw_custom(const uint8_t *a2, lv_color_t accent) {
    static const uint8_t kAlpha[4] = {0, 85, 170, 255};
    for (int y = 0; y < BOARD_AVATAR_PX; ++y) {
        for (int x = 0; x < BOARD_AVATAR_PX; ++x) {
            const int i = y / kCustomScale * kCustomArtPx + x / kCustomScale;
            put_avatar_px(x, y, accent, kAlpha[(a2[i / 4] >> (6 - 2 * (i % 4))) & 3]);
        }
    }
}

void describe_avatar(void) {
    // The descriptor is reused, so drop LVGL's cached decode of the old pixels.
    if (s_avatar_shown != nullptr) lv_image_cache_drop(&s_avatar);
    s_avatar = {};
    s_avatar.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_avatar.header.cf = LV_COLOR_FORMAT_RGB565A8;
    s_avatar.header.w = BOARD_AVATAR_PX;
    s_avatar.header.h = BOARD_AVATAR_PX;
    s_avatar.header.stride = BOARD_AVATAR_PX * 2;
    s_avatar.data_size = sizeof(s_avatar_buf);
    s_avatar.data = s_avatar_buf;
}

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
    if (s_active != &idle_screen() || next == &call_screen()) return true;
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
        if (art == s_custom) draw_custom(s_custom, accent);
        else draw_preset(*static_cast<const avatar_preset_t *>(art), accent);
        describe_avatar();
        s_avatar_shown = art;
        s_avatar_accent = accent;
    }
    return &s_avatar;
}

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
    Screen *next = (state->call_ringing || state->call_active)
                       ? static_cast<Screen *>(&call_screen())
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
    s_active->update(*state);
}
