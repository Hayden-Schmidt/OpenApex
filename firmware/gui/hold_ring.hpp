#pragma once

#include <cstdint>

#include "lvgl.h"

// The hold-to-confirm ring round the panel edge, shared by the call page (accept) and the friend
// prompt (add). While held it traces clockwise from twelve o'clock on an S curve over kHoldMs;
// letting go early retraces the same curve back.
//
// Colour is solid, or a two-colour gradient left to right that runs through black at twelve and
// six ("design/screens/06-add-friend": our accent on the left, the friend's on the right,
// through black so the two never mix to grey).
class HoldRing {
public:
    using Done = void (*)(void *ctx);

    // The input device's long-press time (touch_driver.c, lv_indev_set_long_press_time), so every
    // hold on the terminal takes the same time.
    static constexpr uint32_t kHoldMs = 2000;

    // Draws on a new full-size child of `parent`, `stroke` px wide with its outer edge on the panel
    // edge. The child takes no presses: they stay with the page.
    void create(lv_obj_t *parent, int32_t diameter, int32_t stroke);

    void set_color(lv_color_t color);
    void set_gradient(lv_color_t left, lv_color_t right);

    // Runs the hold *time* toward `to` (0..1000) linearly; the drawn sweep is eased from it, so
    // reversing part-way retraces exactly the curve drawn so far. `done` fires on arrival.
    void run(int32_t to, Done done = nullptr, void *ctx = nullptr);

    // Empty, no run in progress.
    void reset();

    int32_t t() const { return t_; }
    lv_obj_t *obj() const { return obj_; }

private:
    static void exec(void *var, int32_t value);
    static void completed(lv_anim_t *a);
    static void draw_cb(lv_event_t *e);

    lv_obj_t *obj_ = nullptr;
    int32_t stroke_ = 1;
    int32_t t_ = 0;  // 0..1000
    lv_color_t left_ = lv_color_white();
    lv_color_t right_ = lv_color_white();
    bool gradient_ = false;
    Done done_ = nullptr;
    void *done_ctx_ = nullptr;
};
