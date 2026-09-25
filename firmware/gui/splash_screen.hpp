#pragma once

#include "screen.hpp"

// design reference/1. Startup Screen/bootscreen.md + "Boot Screen.svg".
//
// The power-on page: black field, centred logo, held for a minimum of 2s while the rest of the
// terminal starts. gui_app.cpp owns that hold and routes to the real page once it expires -- this
// class only draws.
//
// The logo cannot be baked into the panel: the GC9A01 (and the S3's CO5300) have no nonvolatile
// store and show nothing until the MCU clocks the init sequence out over SPI, so "boot screen" is
// necessarily software. See the page doc for why the bootloader-side alternative was rejected.
//
// BASIC and RICH render the same thing today. The spec's richer logo animation is deferred until
// the real logo asset exists -- the placeholder is `cat-svgrepo-com.svg`.
class SplashScreen : public Screen {
public:
    SplashScreen();
    void update(const terminal_view_state_t &state) override;

private:
    lv_obj_t *logo_ = nullptr;
};
