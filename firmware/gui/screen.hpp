#pragma once

#include "lvgl.h"
#include "view_state.h"

// Base for a full-screen page (§16.6 Layer 1/2). Each subclass owns its own top-level lv_obj_t,
// created in its constructor, so LVGL's screen-transition API (lv_screen_load / lv_screen_load_anim)
// can operate between separate Screen instances.
class Screen {
public:
    Screen() : root_(lv_obj_create(nullptr)) {}
    virtual ~Screen() { lv_obj_delete(root_); }

    Screen(const Screen &) = delete;
    Screen &operator=(const Screen &) = delete;

    lv_obj_t *root() const { return root_; }

    // Called every frame while this screen is active.
    virtual void update(const terminal_view_state_t &state) = 0;

protected:
    lv_obj_t *root_;
};
