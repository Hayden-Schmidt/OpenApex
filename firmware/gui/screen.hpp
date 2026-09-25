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

    // Page transitions ("design reference/Screen Transitions.md"). The theme loads a page and calls
    // enter(); gui_app.cpp calls leave() and loads the next page only once `done` fires. The
    // defaults are instant, for pages with no motion of their own.
    using LeaveDone = void (*)(void *ctx);
    virtual void enter() {}
    virtual void leave(LeaveDone done, void *ctx) { done(ctx); }

    // True once enter()'s incoming sequence has fully landed.
    virtual bool entered() const { return true; }

    // A page that animates in *over* the previous one returns true: the theme then leaves the
    // previous page loaded and this page loads its own root when its reveal lands.
    virtual bool enters_over() const { return false; }

protected:
    lv_obj_t *root_;
};
