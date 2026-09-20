// Hardware-less LVGL + SDL PC simulator entry point. Opens an SDL window at exactly the
// configured board display resolution (board_profile.h) and renders the shared gui_screens
// widget tree driven by a fixture terminal_view_state_t sequence -- there is no live BLE/countdown
// data available in this isolated simulator run, so states are scripted rather than sourced from
// countdown_task. The same gui_screens.c is meant to be reused unchanged by the real gui_task in
// firmware/main/main.c once Slice D wires up the physical GC9A01 panel.
#include <cstdio>
#include <cstring>

extern "C" {
#include "lvgl.h"

#include "board_profile.h"

#include "gui_screens.h"
#include "view_state.h"
}

#include <SDL2/SDL.h>

namespace {

// Scripted fixture: cycles through the view states every few seconds so the render pipeline is
// visibly exercised without needing live BLE/countdown input.
terminal_view_state_t make_fixture_frame(uint32_t elapsed_ms) {
    terminal_view_state_t frame;
    std::memset(&frame, 0, sizeof(frame));
    frame.sequence = elapsed_ms;
    frame.speed_kmh_x10 = 450; // 45.0 km/h
    frame.heading_deg = 90;
    frame.battery_percent = 87;

    const uint32_t phase_ms = elapsed_ms % 16000;
    if (phase_ms < 3000) {
        frame.state = VIEW_IDLE;
        frame.icon_type = NAV_ICON_UNKNOWN;
    } else if (phase_ms < 9000) {
        frame.state = VIEW_ACTIVE;
        frame.icon_type = NAV_ICON_TURN_LEFT;
        frame.distance_meters = 400u - ((phase_ms - 3000u) / 10u); // counts down toward the turn
        std::snprintf(frame.street_name, sizeof(frame.street_name), "Elm Street");
        std::snprintf(frame.eta, sizeof(frame.eta), "2 min");
    } else if (phase_ms < 12000) {
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
    (void)argc;
    (void)argv;

    lv_init();

    lv_display_t *disp = lv_sdl_window_create(BOARD_DISP_WIDTH, BOARD_DISP_HEIGHT);
    if (disp == nullptr) {
        std::fprintf(stderr, "sim_lvgl: failed to create SDL/LVGL display\n");
        return 1;
    }
    char title[64];
    std::snprintf(title, sizeof(title), "OpenApex sim_lvgl (%dx%d)", BOARD_DISP_WIDTH,
                  BOARD_DISP_HEIGHT);
    lv_sdl_window_set_title(disp, title);
    lv_sdl_window_set_resizeable(disp, false);
    lv_sdl_mouse_create();

    gui_screens_init();

    std::printf("sim_lvgl: window open at %dx%d (board_profile.h PROTOTYPE_C3_GC9A01)\n",
                BOARD_DISP_WIDTH, BOARD_DISP_HEIGHT);

    // NOTE: do not poll SDL events here. lv_sdl_window_create() installs its own internal LVGL
    // timer (sdl_event_handler) that calls SDL_PollEvent itself on every lv_timer_handler() call --
    // it handles window close/resize/expose and calls exit(0) on SDL_QUIT (LV_SDL_DIRECT_EXIT).
    // A second SDL_PollEvent loop here would race with it and starve it of events (observed:
    // window opens but never repaints past the first frame).
    const uint32_t start_ms = SDL_GetTicks();
    for (;;) {
        uint32_t elapsed = SDL_GetTicks() - start_ms;
        terminal_view_state_t frame = make_fixture_frame(elapsed);
        gui_screens_update(&frame);

        uint32_t idle_ms = lv_timer_handler();
        SDL_Delay(idle_ms < 5 ? 5 : idle_ms);
    }

    return 0;
}
