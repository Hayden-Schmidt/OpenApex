#pragma once

#include "gui_app.hpp"
#include "screen.hpp"

// Incoming call (terminal_view_state_t::call_ringing): the caller, and an Accept button the rider
// taps to answer on the phone. There is no reject -- letting it ring out is the reject, and the
// phone's CLEAR takes this page down.
//
// PLACEHOLDER LAYOUT. The real design comes from "design reference/5. Phone App" / a dedicated
// call-page reference; this exists so the data and accept paths can be proven end to end first.
class CallScreen : public Screen {
public:
    CallScreen();

    void update(const terminal_view_state_t &state) override;

    void set_accept_handler(gui_call_accept_cb_t cb);

private:
    static void accept_clicked_cb(lv_event_t *e);

    int32_t px(float ref_240) const;

    float scale_ = 1.0f;
    gui_call_accept_cb_t accept_cb_ = nullptr;

    lv_obj_t *title_label_ = nullptr;   // "Incoming call" / "Answering..."
    lv_obj_t *caller_label_ = nullptr;
    lv_obj_t *accept_btn_ = nullptr;

    uint16_t call_id_ = 0;
    bool answering_ = false;          // tapped for call_id_; waits for the phone's CLEAR
    char last_caller_[CALL_CALLER_LEN] = {0};
};
