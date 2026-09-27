#pragma once

#include "gui_app.hpp"
#include "screen.hpp"

// design reference/6. Calling/OpenApex Call Screens.svg.
//
// Incoming and answered call (terminal_view_state_t::call_ringing / call_active): caller name above
// a theme-coloured bubble holding the number, a phone glyph below it. There is no reject -- letting
// it ring out is the reject, and the phone's CLEAR takes this page down.
//
// Motion is the idle page's, element for element (idle_screen.hpp): the bubble rises in from below
// the panel, the glyph slides out from behind it, then the bubble drops as the caller rises out from
// behind its top edge (clipped by `reveal_`). Leaving reverses it.
//
// Accept is a 2s hold anywhere on the page. While held, a theme-coloured ring traces clockwise
// round the edge on an S curve; letting go early retraces it back. A full ring accepts: the phone
// glyph tucks back up under the bubble and the call duration drops out in its place. When the call
// ends the ring traces back before the idle-style exit plays.
class CallScreen : public Screen {
public:
    CallScreen();
    void update(const terminal_view_state_t &state) override;
    void enter() override;
    void leave(LeaveDone done, void *ctx) override;
    bool entered() const override;
    void set_accept_handler(gui_call_accept_cb_t cb);

private:
    // Positions every widget from progress_, bubble_in_, phone_in_, duration_in_ and ring_t_.
    void apply_layout();
    static void set_progress(void *var, int32_t value);
    static void set_bubble_in(void *var, int32_t value);
    static void set_phone_in(void *var, int32_t value);
    static void set_duration_in(void *var, int32_t value);
    static void set_ring_t(void *var, int32_t value);
    static CallScreen *from_anim(lv_anim_t *a);
    void tween(lv_anim_exec_xcb_t exec, int32_t from, int32_t to, uint32_t full_ms,
               lv_anim_completed_cb_t completed,
               lv_anim_path_cb_t path = lv_anim_path_ease_in_out);

    static void on_press(lv_event_t *e);
    static void on_release(lv_event_t *e);
    void start_hold();
    // Runs the ring toward full (held) or empty (let go); retraces the same curve either way.
    void run_ring(int32_t to, lv_anim_completed_cb_t completed = nullptr);
    // Phone glyph tucks under the bubble, then the duration drops out. Once per call.
    void show_answered();
    void set_caller(const char *caller, const char *number);

    int32_t px(float ref_240) const;

    float scale_ = 1.0f;
    int32_t width_ = 240;
    gui_call_accept_cb_t accept_cb_ = nullptr;

    lv_obj_t *reveal_ = nullptr;          // clip window: screen top .. bubble top edge
    lv_obj_t *caller_label_ = nullptr;    // inside reveal_
    lv_obj_t *bubble_ = nullptr;
    lv_obj_t *number_label_ = nullptr;    // inside bubble_
    lv_obj_t *phone_icon_ = nullptr;
    lv_obj_t *duration_label_ = nullptr;
    lv_obj_t *ring_ = nullptr;

    int32_t progress_ = 0;     // 0..1000, bubble high .. caller risen, bubble dropped
    int32_t bubble_in_ = 0;    // 0..1000, bubble below the panel .. at rest
    int32_t phone_in_ = 0;     // 0..1000, glyph behind the bubble .. at rest
    int32_t duration_in_ = 0;  // 0..1000, duration behind the bubble .. at rest
    int32_t ring_t_ = 0;       // 0..1000 hold *time*, linear; the drawn sweep is eased from it
    bool landed_ = false;      // entry finished
    bool leaving_ = false;
    bool pressed_ = false;
    bool accepted_ = false;    // ring filled; accept sent for call_id_
    bool answered_ = false;    // show_answered() has run for this call
    bool active_ = false;      // phone reports the call answered
    bool ringing_ = false;
    LeaveDone done_ = nullptr;
    void *done_ctx_ = nullptr;
    int collapse_left_ = 0;    // leave(): caller and glyph collapses still running
    uint16_t call_id_ = 0;
    uint32_t last_elapsed_s_ = UINT32_MAX;
    char last_caller_[CALL_CALLER_LEN] = {0};
    char last_number_[CALL_NUMBER_LEN] = {0};
};
