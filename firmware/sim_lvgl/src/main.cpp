// Hardware-less LVGL + SDL PC simulator entry point. Opens an SDL window at exactly the
// configured board display resolution (board_profile.h), driven by a fixture terminal_view_state_t
// sequence -- there is no live BLE/countdown data available in this isolated simulator run, so
// states are scripted rather than sourced from countdown_task. The GUI itself is the same
// firmware/gui/ C++ code (gui_app_init/gui_app_update, docs/OpenApex_SPEC.md §16.6) reused
// unchanged by the real gui_task in firmware/main/main.c.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

extern "C" {
#include "lvgl.h"

#include "board_profile.h"

#include "view_state.h"
}

#include "gui_app.hpp"
#include "shot.hpp"

#include <SDL2/SDL.h>

namespace {

// Scripted fixture: cycles through the view states, and through a spread of nav_icon_t maneuvers
// while ACTIVE, so the render pipeline (including NavRenderer's maneuver-change tween) is visibly
// exercised without needing live BLE/countdown input.
// Idle-page fixture (--page idle): pins VIEW_IDLE and flips the phone link every 5s so the
// connect/disconnect transition described in "design reference/2. Idle Screen/2. idle screen.md"
// replays continuously for review. The clock ticks in real time from the host so the digits are
// visibly live rather than a frozen string.
terminal_view_state_t make_idle_fixture_frame(uint32_t elapsed_ms) {
    terminal_view_state_t frame;
    std::memset(&frame, 0, sizeof(frame));
    frame.sequence = elapsed_ms;
    frame.state = VIEW_IDLE;
    frame.icon_type = NAV_ICON_UNKNOWN;
    frame.speed_kmh_x10 = 0xFFFF;
    frame.heading_deg = 0xFFFF;
    frame.battery_percent = 87;
    frame.odometer_meters = 3500000u; // 3,500 km, the value in the Figma reference
    frame.phone_connected = ((elapsed_ms / 5000u) % 2u) == 1u;

    const std::time_t now = std::time(nullptr);
    const std::tm *lt = std::localtime(&now);
    if (lt != nullptr) {
        std::snprintf(frame.clock, sizeof(frame.clock), "%02d:%02d", lt->tm_hour, lt->tm_min);
    }
    return frame;
}

// Odometer-page fixture (--page odometer): pins VIEW_IDLE (the odometer is a rider-selected page,
// not a nav state -- see gui_app_set_page_override) and sweeps the heading a full turn every 20s so
// the shared compass ring and the degree/cardinal readout are both visibly live. The odometer count
// climbs about a tenth of a km per second so the digits actually tick over during a review.
terminal_view_state_t make_odometer_fixture_frame(uint32_t elapsed_ms) {
    terminal_view_state_t frame;
    std::memset(&frame, 0, sizeof(frame));
    frame.sequence = elapsed_ms;
    frame.state = VIEW_IDLE;
    frame.icon_type = NAV_ICON_UNKNOWN;
    frame.speed_kmh_x10 = 0xFFFF;
    frame.battery_percent = 87;
    frame.phone_connected = true;
    frame.heading_deg = static_cast<uint16_t>((elapsed_ms / 55u) % 360u);
    frame.odometer_meters = 99000u + elapsed_ms * 100u;

    const std::time_t now = std::time(nullptr);
    const std::tm *lt = std::localtime(&now);
    if (lt != nullptr) {
        std::snprintf(frame.clock, sizeof(frame.clock), "%02d:%02d", lt->tm_hour, lt->tm_min);
    }
    return frame;
}

terminal_view_state_t make_fixture_frame(uint32_t elapsed_ms) {
    terminal_view_state_t frame;
    std::memset(&frame, 0, sizeof(frame));
    frame.sequence = elapsed_ms;
    frame.speed_kmh_x10 = 450; // 45.0 km/h
    frame.heading_deg = 90;
    frame.battery_percent = 87;

    static const nav_icon_t kActiveIcons[] = {
        NAV_ICON_STRAIGHT,          NAV_ICON_TURN_LEFT,          NAV_ICON_TURN_RIGHT,
        NAV_ICON_SLIGHT_LEFT,       NAV_ICON_SLIGHT_RIGHT,       NAV_ICON_SHARP_LEFT,
        NAV_ICON_SHARP_RIGHT,       NAV_ICON_ROUNDABOUT_LEFT,    NAV_ICON_ROUNDABOUT_RIGHT,
        NAV_ICON_ROUNDABOUT_STRAIGHT, NAV_ICON_U_TURN,
    };
    constexpr uint32_t kActivePhaseMs = 3000;
    constexpr uint32_t kActiveTotalMs =
        kActivePhaseMs * (sizeof(kActiveIcons) / sizeof(kActiveIcons[0]));
    constexpr uint32_t kIdleMs = 3000;
    constexpr uint32_t kStaleMs = 3000;
    constexpr uint32_t kArrivedMs = 3000;
    constexpr uint32_t kCycleMs = kIdleMs + kActiveTotalMs + kStaleMs + kArrivedMs;

    const uint32_t phase_ms = elapsed_ms % kCycleMs;
    if (phase_ms < kIdleMs) {
        frame.state = VIEW_IDLE;
        frame.icon_type = NAV_ICON_UNKNOWN;
    } else if (phase_ms < kIdleMs + kActiveTotalMs) {
        const uint32_t active_ms = phase_ms - kIdleMs;
        const size_t idx = (active_ms / kActivePhaseMs) % (sizeof(kActiveIcons) / sizeof(kActiveIcons[0]));
        frame.state = VIEW_ACTIVE;
        frame.icon_type = kActiveIcons[idx];
        const uint32_t elapsed_in_phase = active_ms % kActivePhaseMs;
        frame.distance_meters = elapsed_in_phase < 400u ? 400u - elapsed_in_phase : 0u;
        std::snprintf(frame.street_name, sizeof(frame.street_name), "Elm Street");
        std::snprintf(frame.eta, sizeof(frame.eta), "2 min");
    } else if (phase_ms < kIdleMs + kActiveTotalMs + kStaleMs) {
        frame.state = VIEW_STALE;
        frame.stale = true;
        frame.icon_type = NAV_ICON_TURN_LEFT;
        frame.distance_meters = 40;
        std::snprintf(frame.street_name, sizeof(frame.street_name), "Elm Street");
    } else {
        frame.state = VIEW_ARRIVED;
        frame.icon_type = NAV_ICON_ARRIVED;
    }
    return frame;
}

} // namespace

namespace {

void print_usage(const char *exe) {
    std::fprintf(stderr,
                 "usage: %s [width [height]] [--page <name>] [--shot <ms> <file.png>]\n"
                 "  width/height   panel resolution override (default: board_profile.h)\n"
                 "  --page         fixture to run: 'all' (default, full state cycle) or 'idle'\n"
                 "  --shot         render until <ms> of fixture time, write <file.png>, exit\n",
                 exe);
}

} // namespace

int main(int argc, char *argv[]) {
    // Optional runtime override of the panel resolution, e.g. `program.exe 466` for the RICH/S3
    // tier's 466x466 without needing a separate PlatformIO env/rebuild. Defaults to this env's
    // compiled BOARD_DISP_WIDTH/HEIGHT (board_profile.h). firmware/gui itself never reads these
    // macros for sizing (see dial_screen.cpp) -- it queries the actual lv_display resolution set
    // here, so it works unmodified at whatever size the window is created at.
    int32_t disp_w = BOARD_DISP_WIDTH;
    int32_t disp_h = BOARD_DISP_HEIGHT;
    // --shot turns the sim into a one-frame renderer for design review: run the normal loop until
    // the fixture reaches `shot_at_ms`, dump the screen, exit. The output path is always
    // overwritten, never suffixed, so repeated runs don't accumulate PNGs in the tree.
    uint32_t shot_at_ms = 0;
    const char *shot_path = nullptr;
    bool idle_page = false;
    bool odometer_page = false;
    int positional = 0;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--page") == 0) {
            if (i + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            ++i;
            if (std::strcmp(argv[i], "idle") == 0) {
                idle_page = true;
            } else if (std::strcmp(argv[i], "odometer") == 0) {
                odometer_page = true;
            } else if (std::strcmp(argv[i], "all") != 0) {
                std::fprintf(stderr, "sim_lvgl: unknown page '%s'\n", argv[i]);
                print_usage(argv[0]);
                return 1;
            }
        } else if (std::strcmp(argv[i], "--shot") == 0) {
            if (i + 2 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            shot_at_ms = static_cast<uint32_t>(std::atol(argv[i + 1]));
            shot_path = argv[i + 2];
            i += 2;
        } else if (argv[i][0] == '-') {
            print_usage(argv[0]);
            return 1;
        } else if (positional == 0) {
            disp_w = disp_h = std::atoi(argv[i]);
            ++positional;
        } else if (positional == 1) {
            disp_h = std::atoi(argv[i]);
            ++positional;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (disp_w <= 0 || disp_h <= 0) {
        std::fprintf(stderr, "sim_lvgl: invalid resolution argument\n");
        return 1;
    }

    // Windows-only: without this, SDL_WINDOWPOS_UNDEFINED (used by lv_sdl_window_create below)
    // is computed in OS-scaled coordinates while the window itself is created at physical-pixel
    // size, so on a scaled display the window can land partly or fully off-screen. Must be set
    // before any SDL window/video call.
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "system");

    lv_init();

    lv_display_t *disp = lv_sdl_window_create(disp_w, disp_h);
    if (disp == nullptr) {
        std::fprintf(stderr, "sim_lvgl: failed to create SDL/LVGL display\n");
        return 1;
    }
    char title[64];
    std::snprintf(title, sizeof(title), "OpenApex sim_lvgl (%dx%d)", disp_w, disp_h);
    lv_sdl_window_set_title(disp, title);
    lv_sdl_window_set_resizeable(disp, false);
    lv_sdl_mouse_create();

    // lv_sdl_window_create() positions the window via SDL_WINDOWPOS_UNDEFINED, which on a scaled
    // Windows display can compute a position (or an implied window rect) mostly or entirely off
    // the visible desktop. Re-center explicitly against this display's actual usable bounds
    // instead of trusting UNDEFINED's own math.
    SDL_Window *window = lv_sdl_window_get_window(disp);
    if (window != nullptr) {
        SDL_Rect bounds;
        if (SDL_GetDisplayUsableBounds(0, &bounds) == 0) {
            int win_w = 0;
            int win_h = 0;
            SDL_GetWindowSize(window, &win_w, &win_h);
            SDL_SetWindowPosition(window, bounds.x + (bounds.w - win_w) / 2,
                                   bounds.y + (bounds.h - win_h) / 2);
        } else {
            SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }
    }

    gui_app_init();
    // The odometer is a rider-selected page rather than a view_state_t, so it is reached through
    // the page-override seam, not by feeding a different state.
    if (odometer_page) gui_app_set_page_override(GUI_PAGE_ODOMETER);

    std::printf("sim_lvgl: window open at %dx%d\n", disp_w, disp_h);

    // NOTE: do not poll SDL events here. lv_sdl_window_create() installs its own internal LVGL
    // timer (sdl_event_handler) that calls SDL_PollEvent itself on every lv_timer_handler() call --
    // it handles window close/resize/expose and calls exit(0) on SDL_QUIT (LV_SDL_DIRECT_EXIT).
    // A second SDL_PollEvent loop here would race with it and starve it of events (observed:
    // window opens but never repaints past the first frame).
    const uint32_t start_ms = SDL_GetTicks();
    for (;;) {
        uint32_t elapsed = SDL_GetTicks() - start_ms;
        terminal_view_state_t frame = odometer_page ? make_odometer_fixture_frame(elapsed)
                                      : idle_page    ? make_idle_fixture_frame(elapsed)
                                                     : make_fixture_frame(elapsed);
        gui_app_update(&frame);

        uint32_t idle_ms = lv_timer_handler();

        if (shot_path != nullptr && elapsed >= shot_at_ms) {
            // Force a synchronous redraw before snapshotting: lv_timer_handler() above may have
            // returned with the invalidated areas still pending, which would capture a stale frame.
            lv_refr_now(nullptr);
            const bool ok = shot_write_active_screen(shot_path);
            return ok ? 0 : 1;
        }

        SDL_Delay(idle_ms < 5 ? 5 : idle_ms);
    }

    return 0;
}
