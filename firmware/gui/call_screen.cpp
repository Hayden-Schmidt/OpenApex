#include "call_screen.hpp"

#include <cmath>
#include <cstring>

#include "board_profile.h"
#include "gui_font.hpp"
#include "theme.hpp"

namespace {

constexpr uint32_t kMutedColor = 0x969696;
constexpr float kTitleCy = 62.0f;
constexpr float kCallerCy = 110.0f;
constexpr float kCallerW = 190.0f;  // inside the round panel's chord at kCallerCy
constexpr float kAcceptCy = 180.0f;
constexpr float kAcceptD = 64.0f;   // well past the ~48px minimum for a gloved tap

} // namespace

int32_t CallScreen::px(float ref_240) const { return static_cast<int32_t>(std::lround(ref_240 * scale_)); }

CallScreen::CallScreen() {
    const int32_t width = lv_display_get_horizontal_resolution(lv_display_get_default());
    scale_ = static_cast<float>(width) / 240.0f;

    lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(root_, false);

    title_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(title_label_, montserrat_at_most(px(16)), 0);
    lv_obj_set_style_text_color(title_label_, lv_color_hex(kMutedColor), 0);
    lv_label_set_text(title_label_, "Incoming call");
    lv_obj_align(title_label_, LV_ALIGN_TOP_MID, 0, px(kTitleCy) - px(8));

    caller_label_ = lv_label_create(root_);
    lv_obj_set_width(caller_label_, px(kCallerW));
    lv_label_set_long_mode(caller_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(caller_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(caller_label_, montserrat_at_most(px(28)), 0);
    lv_obj_set_style_text_color(caller_label_, lv_color_white(), 0);
    lv_label_set_text(caller_label_, "");
    lv_obj_align(caller_label_, LV_ALIGN_TOP_MID, 0, px(kCallerCy) - px(14));

    accept_btn_ = lv_button_create(root_);
    lv_obj_set_size(accept_btn_, px(kAcceptD), px(kAcceptD));
    lv_obj_set_style_radius(accept_btn_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(accept_btn_, gui_theme().arrived_surface(), 0);
    lv_obj_set_style_shadow_width(accept_btn_, 0, 0);
    lv_obj_align(accept_btn_, LV_ALIGN_TOP_MID, 0, px(kAcceptCy) - px(kAcceptD) / 2);
    lv_obj_add_event_cb(accept_btn_, accept_clicked_cb, LV_EVENT_CLICKED, this);
    lv_obj_t *accept_text = lv_label_create(accept_btn_);
    lv_obj_set_style_text_font(accept_text, montserrat_at_most(px(14)), 0);
    lv_label_set_text(accept_text, "Accept");
    lv_obj_center(accept_text);
    lv_obj_set_hidden(accept_btn_, true);
}

void CallScreen::set_accept_handler(gui_call_accept_cb_t cb) { accept_cb_ = cb; }

void CallScreen::accept_clicked_cb(lv_event_t *e) {
    auto *self = static_cast<CallScreen *>(lv_event_get_user_data(e));
    if (self->answering_ || self->accept_cb_ == nullptr) return;
    self->answering_ = true;
    self->accept_cb_(self->call_id_);
    lv_label_set_text(self->title_label_, "Answering...");
    lv_obj_set_hidden(self->accept_btn_, true);
}

void CallScreen::update(const terminal_view_state_t &state) {
    if (!state.call_ringing) return;  // leaving: keep showing the call that was just up

    if (state.call_id != call_id_) {
        call_id_ = state.call_id;
        answering_ = false;
        lv_label_set_text(title_label_, "Incoming call");
    }
    if (std::strncmp(last_caller_, state.caller, sizeof(last_caller_)) != 0) {
        std::memcpy(last_caller_, state.caller, sizeof(last_caller_));
        last_caller_[sizeof(last_caller_) - 1] = '\0';
        lv_label_set_text(caller_label_, last_caller_[0] != '\0' ? last_caller_ : "Unknown");
    }

    // Boards without touch show the caller only; the ring plays out on the phone as usual.
    const bool can_accept = BOARD_HAS_TOUCH && accept_cb_ != nullptr && !answering_;
    if (can_accept) {
        lv_obj_set_hidden(accept_btn_, false);
    } else {
        lv_obj_set_hidden(accept_btn_, true);
    }
}
