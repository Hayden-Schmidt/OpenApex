#include "call_screen.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "board_profile.h"
#include "ease.hpp"
#include "gui_font.hpp"
#include "icons.hpp"
#include "theme.hpp"

namespace {

// Geometry lifted from "design reference/6. Calling/OpenApex Call Screens.svg", in its 240px
// reference frame (each of the three frames translated so its circle's centre is at (120, 120)) and
// scaled at runtime, same convention as idle_screen.cpp.
constexpr float kBubbleW = 129.0f;      // minimum; grows to fit a long number
constexpr float kBubbleH = 30.0f;
constexpr float kBubblePadX = 11.0f;    // number text inset from the bubble's ends
constexpr float kBubbleCyRest = 138.0f;
// The bubble starts high and drops as the caller rises out from behind it, the idle page's
// disconnected -> connected move (idle_screen.cpp kBubbleCyDisconnected).
constexpr float kBubbleCyHigh = 122.0f;
constexpr float kDrop = kBubbleCyRest - kBubbleCyHigh;

constexpr float kPhoneCyRest = 174.0f;
constexpr float kDurationCyRest = 168.0f;

// Caller text: two lines at most, 29px line pitch, glyph bottom 8px above the bubble. Glyphs are
// ~21px tall in the SVG, ~30px Montserrat by idle_screen.cpp's glyph-to-font ratio.
constexpr float kCallerFontRef = 30.0f;
constexpr float kCallerPitch = 29.0f;
constexpr float kCallerMaxW = 160.0f;  // the SVG wraps "Ron Ericson" and "Unknown Caller" at this
constexpr float kCallerGap = 4.0f;     // label box bottom (below the glyphs) to bubble top
constexpr int kCallerMaxLines = 2;
constexpr float kNumberFontRef = 15.0f;
constexpr float kDurationFontRef = 15.0f;

// Active Ring: r 117.5, 5px stroke, so its outer edge is the panel edge.
constexpr float kRingW = 5.0f;

// Idle page timings (idle_screen.cpp), so both pages move alike.
constexpr uint32_t kTweenMs = 1000;
constexpr uint32_t kBubbleInMs = 470;
constexpr uint32_t kGlyphInMs = 400;

float lerp(float a, float b, float t) { return a + (b - a) * t; }

// "04:07", or "1:04:07" past the hour.
void format_duration(char *out, size_t out_len, uint32_t s) {
    const unsigned h = static_cast<unsigned>(s / 3600u);
    const unsigned m = static_cast<unsigned>((s / 60u) % 60u);
    const unsigned sec = static_cast<unsigned>(s % 60u);
    if (h > 0) {
        std::snprintf(out, out_len, "%u:%02u:%02u", h, m, sec);
    } else {
        std::snprintf(out, out_len, "%02u:%02u", m, sec);
    }
}

void place_centred(lv_obj_t *obj, int32_t parent_w, int32_t top) {
    lv_obj_set_pos(obj, (parent_w - lv_obj_get_width(obj)) / 2, top);
}

} // namespace

int32_t CallScreen::px(float ref_240) const {
    return static_cast<int32_t>(std::lround(ref_240 * scale_));
}

CallScreen::CallScreen() {
    width_ = lv_display_get_horizontal_resolution(lv_display_get_default());
    scale_ = static_cast<float>(width_) / 240.0f;

    lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(root_, false);
    lv_obj_add_event_cb(root_, on_press, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(root_, on_release, LV_EVENT_RELEASED, this);
    lv_obj_add_event_cb(root_, on_release, LV_EVENT_PRESS_LOST, this);

    const lv_color_t accent = gui_theme().palette();

    // Glyph and duration first, so the bubble covers them: they slide out from *under* it.
    phone_icon_ = lv_image_create(root_);
    lv_image_set_src(phone_icon_, icon_image(ICON_CALL));
    lv_obj_set_style_image_recolor(phone_icon_, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(phone_icon_, LV_OPA_COVER, 0);

    duration_label_ = lv_label_create(root_);
    lv_obj_set_style_text_color(duration_label_, lv_color_white(), 0);
    lv_obj_set_style_text_font(duration_label_, montserrat_at_most(px(kDurationFontRef)), 0);
    lv_label_set_text(duration_label_, "00:00");

    // Clip window for the rising caller, as idle_screen.cpp's reveal_.
    reveal_ = lv_obj_create(root_);
    lv_obj_remove_style_all(reveal_);
    lv_obj_set_width(reveal_, width_);
    lv_obj_set_pos(reveal_, 0, 0);
    lv_obj_set_scrollable(reveal_, false);

    const lv_font_t *caller_font = montserrat_at_most(px(kCallerFontRef));
    caller_label_ = lv_label_create(reveal_);
    lv_obj_set_style_text_color(caller_label_, lv_color_white(), 0);
    lv_obj_set_style_text_font(caller_label_, caller_font, 0);
    lv_obj_set_style_text_align(caller_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(caller_label_,
                                     px(kCallerPitch) - lv_font_get_line_height(caller_font), 0);
    lv_obj_set_width(caller_label_, px(kCallerMaxW));
    lv_label_set_text(caller_label_, "");

    bubble_ = lv_obj_create(root_);
    lv_obj_remove_style_all(bubble_);
    lv_obj_set_size(bubble_, px(kBubbleW), px(kBubbleH));
    lv_obj_set_style_bg_color(bubble_, accent, 0);
    lv_obj_set_style_bg_opa(bubble_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_scrollable(bubble_, false);

    number_label_ = lv_label_create(bubble_);
    lv_obj_set_style_text_color(number_label_, lv_color_black(), 0);
    lv_obj_set_style_text_font(number_label_, montserrat_at_most(px(kNumberFontRef)), 0);
    lv_label_set_text(number_label_, "");

    ring_.create(root_, width_, px(kRingW));
    ring_.set_color(accent);

    // Presses belong to the page, as on the nav page: a clickable child would take them.
    for (lv_obj_t *obj : {reveal_, bubble_}) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_update_layout(root_);
    apply_layout();
}

void CallScreen::set_accept_handler(gui_call_accept_cb_t cb) { accept_cb_ = cb; }

void CallScreen::set_progress(void *var, int32_t value) {
    auto *self = static_cast<CallScreen *>(var);
    self->progress_ = value;
    self->apply_layout();
}

void CallScreen::set_bubble_in(void *var, int32_t value) {
    auto *self = static_cast<CallScreen *>(var);
    self->bubble_in_ = value;
    self->apply_layout();
}

void CallScreen::set_phone_in(void *var, int32_t value) {
    auto *self = static_cast<CallScreen *>(var);
    self->phone_in_ = value;
    self->apply_layout();
}

void CallScreen::set_duration_in(void *var, int32_t value) {
    auto *self = static_cast<CallScreen *>(var);
    self->duration_in_ = value;
    self->apply_layout();
}

CallScreen *CallScreen::from_anim(lv_anim_t *a) { return static_cast<CallScreen *>(a->var); }

// As IdleScreen::tween: duration scales with the distance left, so a reversal from part-way takes
// part of the time.
void CallScreen::tween(lv_anim_exec_xcb_t exec, int32_t from, int32_t to, uint32_t full_ms,
                       lv_anim_completed_cb_t completed, lv_anim_path_cb_t path) {
    const int32_t span = to > from ? to - from : from - to;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, this);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, full_ms * static_cast<uint32_t>(span) / 1000u);
    lv_anim_set_path_cb(&a, path);
    lv_anim_set_completed_cb(&a, completed);
    lv_anim_start(&a);
}

void CallScreen::on_press(lv_event_t *e) {
    auto *self = static_cast<CallScreen *>(lv_event_get_user_data(e));
    if (self->leaving_) return;
    self->pressed_ = true;
    self->start_hold();
}

// A finger already down while the page is still arriving starts the ring once it lands.
void CallScreen::start_hold() {
    if (!pressed_ || !landed_ || leaving_ || !ringing_ || answered_ || accept_cb_ == nullptr) {
        return;
    }
    ring_.run(1000, [](void *ctx) {
        auto *s = static_cast<CallScreen *>(ctx);
        if (!s->pressed_ || s->answered_) return;
        s->accepted_ = true;
        s->accept_cb_(s->call_id_);
        s->show_answered();
    }, this);
}

void CallScreen::on_release(lv_event_t *e) {
    auto *self = static_cast<CallScreen *>(lv_event_get_user_data(e));
    if (!self->pressed_) return;
    self->pressed_ = false;
    if (!self->accepted_ && !self->answered_ && !self->leaving_) self->ring_.run(0);
}

// Accepted here (full ring) or answered on the phone: the ring fills if it had not, the phone glyph
// tucks back under the bubble, then the duration drops out in its place.
void CallScreen::show_answered() {
    if (answered_) return;
    answered_ = true;
    if (ring_.t() < 1000) ring_.run(1000);
    lv_anim_delete(this, set_phone_in);
    tween(set_phone_in, phone_in_, 0, kGlyphInMs, [](lv_anim_t *a) {
        CallScreen *s = from_anim(a);
        s->tween(set_duration_in, s->duration_in_, 1000, kGlyphInMs, nullptr);
    });
}

void CallScreen::enter() {
    lv_anim_delete(this, nullptr);
    leaving_ = false;
    landed_ = false;
    pressed_ = false;
    accepted_ = false;
    answered_ = false;
    bubble_in_ = 0;
    phone_in_ = 0;
    duration_in_ = 0;
    progress_ = 0;
    ring_.reset();
    apply_layout();
    tween(set_bubble_in, 0, 1000, kBubbleInMs, [](lv_anim_t *a) {
        CallScreen *self = from_anim(a);
        self->tween(set_phone_in, 0, 1000, kGlyphInMs, [](lv_anim_t *a2) {
            CallScreen *self2 = from_anim(a2);
            self2->tween(set_progress, 0, 1000, kTweenMs, [](lv_anim_t *a3) {
                CallScreen *self3 = from_anim(a3);
                self3->landed_ = true;
                self3->start_hold();
                // Answered on the phone while the page was still arriving.
                if (self3->active_) self3->show_answered();
            });
        });
    }, anim_path<ease_out_expo>);
}

bool CallScreen::entered() const { return landed_; }

// The ring traces back first, then the idle page's exit: caller and glyph collapse behind the bubble
// together, and the bubble runs off the bottom once both have landed.
void CallScreen::leave(LeaveDone done, void *ctx) {
    lv_anim_delete(this, nullptr);
    leaving_ = true;
    pressed_ = false;
    done_ = done;
    done_ctx_ = ctx;
    ring_.run(0, [](void *ctx) {
        auto *self = static_cast<CallScreen *>(ctx);
        self->collapse_left_ = 3;
        const lv_anim_completed_cb_t collapsed = [](lv_anim_t *a2) {
            CallScreen *s = from_anim(a2);
            if (--s->collapse_left_ > 0) return;
            s->tween(set_bubble_in, s->bubble_in_, 0, kBubbleInMs, [](lv_anim_t *a3) {
                CallScreen *s3 = from_anim(a3);
                s3->done_(s3->done_ctx_);
            }, anim_path<ease_in_expo>);
        };
        // Same length, so they land together.
        self->tween(set_progress, self->progress_, 0, kTweenMs, collapsed);
        self->tween(set_phone_in, self->phone_in_, 0, kTweenMs, collapsed);
        self->tween(set_duration_in, self->duration_in_, 0, kTweenMs, collapsed);
    }, this);
}

void CallScreen::apply_layout() {
    const float t = static_cast<float>(progress_) / 1000.0f;

    const int32_t bubble_h = px(kBubbleH);
    const int32_t rest_cy = px(lerp(kBubbleCyHigh, kBubbleCyRest, t));
    const int32_t rest_top = rest_cy - bubble_h / 2;
    const int32_t entry_dy = static_cast<int32_t>(
        std::lround((width_ - rest_top) * (1.0f - static_cast<float>(bubble_in_) / 1000.0f)));
    const int32_t bubble_cy = rest_cy + entry_dy;
    lv_obj_set_pos(bubble_, (width_ - lv_obj_get_width(bubble_)) / 2, rest_top + entry_dy);

    // The clip window's bottom edge is the bubble's resting top edge (see idle_screen.cpp).
    lv_obj_set_height(reveal_, rest_top > 0 ? rest_top : 0);

    // The caller starts with its top on the high bubble's top edge -- wholly clipped, whatever its
    // line count -- and rises to sit its bottom kCallerGap above the resting bubble.
    const int32_t caller_h = lv_obj_get_height(caller_label_);
    const float hidden_top = static_cast<float>(px(kBubbleCyHigh) - bubble_h / 2);
    const float shown_top =
        static_cast<float>(px(kBubbleCyRest) - bubble_h / 2 - px(kCallerGap) - caller_h);
    place_centred(caller_label_, width_, static_cast<int32_t>(std::lround(lerp(hidden_top, shown_top, t))));

    // Each glyph starts tucked behind the bubble's centre and slides out to its resting place.
    const auto glyph_cy = [&](float rest_ref, int32_t in) {
        const int32_t rest = px(lerp(rest_ref - kDrop, rest_ref, t)) + entry_dy;
        return static_cast<int32_t>(std::lround(lerp(static_cast<float>(bubble_cy),
                                                     static_cast<float>(rest),
                                                     static_cast<float>(in) / 1000.0f)));
    };
    place_centred(phone_icon_, width_,
                  glyph_cy(kPhoneCyRest, phone_in_) - lv_obj_get_height(phone_icon_) / 2);
    place_centred(duration_label_, width_,
                  glyph_cy(kDurationCyRest, duration_in_) - lv_obj_get_height(duration_label_) / 2);
}

void CallScreen::set_caller(const char *caller, const char *number) {
    std::memcpy(last_caller_, caller, sizeof(last_caller_));
    last_caller_[sizeof(last_caller_) - 1] = '\0';
    std::memcpy(last_number_, number, sizeof(last_number_));
    last_number_[sizeof(last_number_) - 1] = '\0';

    // The phone sends an empty name when the dialer only knows the number.
    lv_label_set_long_mode(caller_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(caller_label_, LV_SIZE_CONTENT);
    lv_label_set_text(caller_label_, last_caller_[0] != '\0' ? last_caller_ : "Unknown Caller");
    // Empty number: withheld, or a saved contact whose number the phone could not read.
    lv_label_set_text(number_label_, last_number_[0] != '\0' ? last_number_
                                     : last_caller_[0] != '\0' ? "Saved contact"
                                                               : "Private number");
    lv_obj_update_layout(root_);

    // Two lines at most; a longer name ends in an ellipsis.
    const int32_t max_h = px(kCallerPitch) * kCallerMaxLines;
    if (lv_obj_get_height(caller_label_) > max_h + px(kCallerPitch) / 2) {
        lv_label_set_long_mode(caller_label_, LV_LABEL_LONG_DOT);
        lv_obj_set_height(caller_label_, max_h);
    }

    const int32_t min_w = px(kBubbleW);
    const int32_t fit_w = lv_obj_get_width(number_label_) + 2 * px(kBubblePadX);
    lv_obj_set_width(bubble_, fit_w > min_w ? fit_w : min_w);
    lv_obj_center(number_label_);
    lv_obj_update_layout(root_);
    apply_layout();
}

void CallScreen::update(const terminal_view_state_t &state) {
    // Leaving: keep showing the call that was just up.
    if (leaving_ || !(state.call_ringing || state.call_active)) return;

    if (state.call_id != call_id_) {
        // A different call on a page already up (a second call after the first ended mid-exit).
        call_id_ = state.call_id;
        accepted_ = false;
        pressed_ = false;
        if (answered_) {
            answered_ = false;
            lv_anim_delete(this, set_phone_in);
            lv_anim_delete(this, set_duration_in);
            tween(set_duration_in, duration_in_, 0, kGlyphInMs, nullptr);
            tween(set_phone_in, phone_in_, 1000, kGlyphInMs, nullptr);
        }
        if (landed_) ring_.run(0);
    }
    ringing_ = state.call_ringing;
    active_ = state.call_active;

    if (std::strncmp(last_caller_, state.caller, sizeof(last_caller_)) != 0 ||
        std::strncmp(last_number_, state.call_number, sizeof(last_number_)) != 0) {
        set_caller(state.caller, state.call_number);
    }

    const uint32_t elapsed = state.call_active ? state.call_elapsed_s : 0;
    if (elapsed != last_elapsed_s_) {
        last_elapsed_s_ = elapsed;
        char text[16];
        format_duration(text, sizeof(text), elapsed);
        lv_label_set_text(duration_label_, text);
        lv_obj_update_layout(duration_label_);
        apply_layout();  // re-centre: the text's width changes with it
    }

    if (active_ && landed_) show_answered();
}
