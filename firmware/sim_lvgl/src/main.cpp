// Hardware-less LVGL + SDL PC simulator entry point. Opens an SDL window at exactly the
// configured board display resolution (board_profile.h), driven by a fixture terminal_view_state_t
// sequence -- there is no live BLE/countdown data available in this isolated simulator run, so
// states are scripted rather than sourced from countdown_task. The GUI itself is the same
// firmware/gui/ C++ code (gui_app_init/gui_app_update, docs/OpenApex_SPEC.md §16.6) reused
// unchanged by the real gui_task in firmware/main/main.c.
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "lvgl.h"

#include "board_profile.h"

#include "view_state.h"
}

#include "gui_app.hpp"

#include <SDL2/SDL.h>

namespace {

// Scripted fixture: cycles through the view states, and through a spread of nav_icon_t maneuvers
// while ACTIVE, so the render pipeline (including NavRenderer's maneuver-change tween) is visibly
// exercised without needing live BLE/countdown input.
terminal_view_state_t make_fixture_frame(uint32_t elapsed_ms) {
    terminal_view_state_t frame;
    std::memset(&frame, 0, sizeof(frame));
    frame.sequence = elapsed_ms;
    frame.speed_kmh_x10 = 450; // 45.0 km/h
    frame.heading_deg = 90;
    frame.battery_percent = 87;

    static const nav_icon_t kActiveIcons[] = {
        NAV_ICON_STRAIGHT,      NAV_ICON_TURN_LEFT,       NAV_ICON_TURN_RIGHT,
        NAV_ICON_SLIGHT_LEFT,   NAV_ICON_SLIGHT_RIGHT,    NAV_ICON_SHARP_LEFT,
        NAV_ICON_SHARP_RIGHT,   NAV_ICON_ROUNDABOUT,      NAV_ICON_U_TURN,
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

int main(int argc, char *argv[]) {
    // Optional runtime override of the panel resolution, e.g. `program.exe 466` for the RICH/S3
    // tier's 466x466 without needing a separate PlatformIO env/rebuild. Defaults to this env's
    // compiled BOARD_DISP_WIDTH/HEIGHT (board_profile.h). firmware/gui itself never reads these
    // macros for sizing (see dial_screen.cpp) -- it queries the actual lv_display resolution set
    // here, so it works unmodified at whatever size the window is created at.
    int32_t disp_w = BOARD_DISP_WIDTH;
    int32_t disp_h = BOARD_DISP_HEIGHT;
    if (argc >= 2) {
        disp_w = disp_h = std::atoi(argv[1]);
    }
    if (argc >= 3) {
        disp_h = std::atoi(argv[2]);
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

    std::printf("sim_lvgl: window open at %dx%d\n", disp_w, disp_h);

    // NOTE: do not poll SDL events here. lv_sdl_window_create() installs its own internal LVGL
    // timer (sdl_event_handler) that calls SDL_PollEvent itself on every lv_timer_handler() call --
    // it handles window close/resize/expose and calls exit(0) on SDL_QUIT (LV_SDL_DIRECT_EXIT).
    // A second SDL_PollEvent loop here would race with it and starve it of events (observed:
    // window opens but never repaints past the first frame).
    const uint32_t start_ms = SDL_GetTicks();
    for (;;) {
        uint32_t elapsed = SDL_GetTicks() - start_ms;
        terminal_view_state_t frame = make_fixture_frame(elapsed);
        gui_app_update(&frame);

        uint32_t idle_ms = lv_timer_handler();
        SDL_Delay(idle_ms < 5 ? 5 : idle_ms);
    }

    return 0;
}
