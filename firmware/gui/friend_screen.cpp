#include "friend_screen.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "avatar_art.hpp"
#include "avatars_data.h"
#include "ease.hpp"
#include "gui_font.hpp"

namespace {

// Geometry from "design reference/7. Add Freind Screen" in its 240px frame, scaled at runtime.
constexpr float kTitleCy = 78.0f;
constexpr float kAvatarCy = 127.0f;
constexpr float kAvatarPer240 = 64.0f;   // on-screen character size (2x the 32px art at 240)
constexpr float kPairGap = 8.0f;         // between the two characters when side by side
constexpr float kHintCy = 170.0f;
constexpr float kTitleFontRef = 30.0f;
constexpr float kHintFontRef = 15.0f;
constexpr float kRingW = 5.0f;           // as the call page's
constexpr float kRise = 24.0f;           // entry rise, px at 240

constexpr uint32_t kInMs = 500;
constexpr uint32_t kPairMs = 600;
constexpr uint32_t kFadeMs = 300;

float lerp(float a, float b, float t) { return a + (b - a) * t; }

void place_centred(lv_obj_t *obj, int32_t cx, int32_t cy) {
    lv_obj_set_pos(obj, cx - lv_obj_get_width(obj) / 2, cy - lv_obj_get_height(obj) / 2);
}

} // namespace

int32_t FriendScreen::px(float ref_240) const {
    return static_cast<int32_t>(std::lround(ref_240 * scale_));
}

FriendScreen::FriendScreen() {
    width_ = lv_display_get_horizontal_resolution(lv_display_get_default());
    scale_ = static_cast<float>(width_) / 240.0f;
    avatar_zoom_ = static_cast<int32_t>(std::lround(kAvatarPer240 * scale_ / AVATAR_ART_PX));
    if (avatar_zoom_ < 1) avatar_zoom_ = 1;

    lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(root_, false);
    lv_obj_add_event_cb(root_, on_press, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(root_, on_release, LV_EVENT_RELEASED, this);
    lv_obj_add_event_cb(root_, on_release, LV_EVENT_PRESS_LOST, this);

    title_ = lv_label_create(root_);
    lv_obj_set_style_text_color(title_, lv_color_white(), 0);
    lv_obj_set_style_text_font(title_, montserrat_at_most(px(kTitleFontRef)), 0);
    lv_label_set_text(title_, "");

    hint_ = lv_label_create(root_);
    lv_obj_set_style_text_font(hint_, montserrat_at_most(px(kHintFontRef)), 0);
    lv_label_set_text(hint_, "");

    // Nearest-neighbour zoom keeps the pixel art crisp.
    for (lv_obj_t **img : {&self_img_, &peer_img_}) {
        *img = lv_image_create(root_);
        lv_image_set_antialias(*img, false);
        lv_image_set_scale(*img, static_cast<uint32_t>(avatar_zoom_ * LV_SCALE_NONE));
        lv_obj_set_size(*img, AVATAR_ART_PX * avatar_zoom_, AVATAR_ART_PX * avatar_zoom_);
        lv_image_set_inner_align(*img, LV_IMAGE_ALIGN_CENTER);
    }

    ring_.create(root_, width_, px(kRingW));

    for (lv_obj_t *obj : {title_, hint_, self_img_, peer_img_}) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    }
    apply_layout();
}

void FriendScreen::set_accept_handler(gui_friend_accept_cb_t cb) { accept_cb_ = cb; }

void FriendScreen::set_in(void *var, int32_t value) {
    auto *self = static_cast<FriendScreen *>(var);
    self->in_ = value;
    self->apply_layout();
}

void FriendScreen::set_pair(void *var, int32_t value) {
    auto *self = static_cast<FriendScreen *>(var);
    self->pair_ = value;
    self->apply_layout();
}

void FriendScreen::set_fade(void *var, int32_t value) {
    auto *self = static_cast<FriendScreen *>(var);
    self->fade_ = value;
    self->apply_layout();
}

FriendScreen *FriendScreen::from_anim(lv_anim_t *a) { return static_cast<FriendScreen *>(a->var); }

// As CallScreen::tween: duration scales with the distance left.
void FriendScreen::tween(lv_anim_exec_xcb_t exec, int32_t from, int32_t to, uint32_t ms,
                         lv_anim_completed_cb_t completed, lv_anim_path_cb_t path) {
    const int32_t span = to > from ? to - from : from - to;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, this);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms * static_cast<uint32_t>(span) / 1000u);
    lv_anim_set_path_cb(&a, path);
    lv_anim_set_completed_cb(&a, completed);
    lv_anim_start(&a);
}

void FriendScreen::on_press(lv_event_t *e) {
    auto *self = static_cast<FriendScreen *>(lv_event_get_user_data(e));
    if (self->leaving_) return;
    self->pressed_ = true;
    self->start_hold();
}

void FriendScreen::start_hold() {
    if (!pressed_ || !landed_ || leaving_ || accepted_ || shown_ != MESH_PROMPT_OFFER ||
        accept_cb_ == nullptr) {
        return;
    }
    ring_.run(1000, [](void *ctx) {
        auto *s = static_cast<FriendScreen *>(ctx);
        if (!s->pressed_ || s->accepted_) return;
        s->accepted_ = true;
        s->accept_cb_(s->id_);
    }, this);
}

void FriendScreen::on_release(lv_event_t *e) {
    auto *self = static_cast<FriendScreen *>(lv_event_get_user_data(e));
    if (!self->pressed_) return;
    self->pressed_ = false;
    if (!self->accepted_ && !self->leaving_ && self->shown_ == MESH_PROMPT_OFFER) self->ring_.run(0);
}

void FriendScreen::enter() {
    lv_anim_delete(this, nullptr);
    leaving_ = false;
    landed_ = false;
    pressed_ = false;
    accepted_ = false;
    in_ = 0;
    pair_ = 0;
    fade_ = 1000;
    ring_.reset();
    apply_layout();
    tween(set_in, 0, 1000, kInMs, [](lv_anim_t *a) {
        FriendScreen *s = from_anim(a);
        s->landed_ = true;
        s->start_hold();
    }, anim_path<ease_out_expo>);
}

bool FriendScreen::entered() const { return landed_; }

// An unfinished ring traces back, then the page fades out. Done keeps its full ring to the end.
void FriendScreen::leave(LeaveDone done, void *ctx) {
    lv_anim_delete(this, set_in);
    lv_anim_delete(this, set_pair);
    leaving_ = true;
    pressed_ = false;
    done_ = done;
    done_ctx_ = ctx;
    const HoldRing::Done fade = [](void *c) {
        auto *s = static_cast<FriendScreen *>(c);
        s->tween(set_fade, s->fade_, 0, kFadeMs, [](lv_anim_t *a) {
            FriendScreen *s2 = from_anim(a);
            s2->done_(s2->done_ctx_);
        });
    };
    if (shown_ == MESH_PROMPT_DONE) {
        fade(this);
    } else {
        ring_.run(0, fade, this);
    }
}

void FriendScreen::apply_layout() {
    const float in = static_cast<float>(in_) / 1000.0f;
    const float pair = static_cast<float>(pair_) / 1000.0f;
    const int32_t dy = px(kRise * (1.0f - in));
    const lv_opa_t opa = static_cast<lv_opa_t>(255 * in_ / 1000 * fade_ / 1000);
    const int32_t cx = width_ / 2;

    place_centred(title_, cx, px(kTitleCy) + dy);
    place_centred(hint_, cx, px(kHintCy) + dy);

    // Side by side: ours on the left, theirs on the right, the pair centred.
    const float half = (static_cast<float>(AVATAR_ART_PX * avatar_zoom_) + px(kPairGap)) / 2.0f;
    const int32_t acy = px(kAvatarCy) + dy;
    place_centred(peer_img_, static_cast<int32_t>(std::lround(lerp(cx, cx + half, pair))), acy);
    place_centred(self_img_, static_cast<int32_t>(std::lround(lerp(cx, cx - half, pair))), acy);

    for (lv_obj_t *obj : {title_, hint_, peer_img_}) lv_obj_set_style_opa(obj, opa, 0);
    lv_obj_set_style_opa(self_img_, static_cast<lv_opa_t>(opa * pair_ / 1000), 0);
    lv_obj_set_style_opa(ring_.obj(), opa, 0);
}

void FriendScreen::set_prompt(const terminal_view_state_t &state) {
    std::memcpy(id_, state.friend_prompt_id, sizeof(id_));
    peer_ = state.friend_peer;
    self_ = state.friend_self;
    const lv_color_t peer_accent = lv_color_hex(peer_.accent_rgb);
    const lv_color_t self_accent = lv_color_hex(self_.accent_rgb);

    avatar_art_draw(peer_buf_, AVATAR_ART_PX, peer_.avatar, peer_.has_custom ? peer_.art : nullptr,
                    peer_accent);
    avatar_art_describe(&peer_dsc_, peer_buf_, AVATAR_ART_PX, AVATAR_ART_PX);
    lv_image_set_src(peer_img_, &peer_dsc_);
    avatar_art_draw(self_buf_, AVATAR_ART_PX, self_.avatar, self_.has_custom ? self_.art : nullptr,
                    self_accent);
    avatar_art_describe(&self_dsc_, self_buf_, AVATAR_ART_PX, AVATAR_ART_PX);
    lv_image_set_src(self_img_, &self_dsc_);

    ring_.set_gradient(self_accent, peer_accent);
    lv_obj_set_style_text_color(hint_, peer_accent, 0);
    shown_ = MESH_PROMPT_NONE;
}

void FriendScreen::show_state(mesh_prompt_state_t st) {
    if (st == shown_) return;
    shown_ = st;
    char text[MESH_NAME_MAX + 16];
    switch (st) {
    case MESH_PROMPT_OFFER:
        lv_label_set_text(title_, peer_.name);
        lv_label_set_text(hint_, "Hold to Accept");
        break;
    case MESH_PROMPT_WAITING:
        lv_label_set_text(title_, peer_.name);
        std::snprintf(text, sizeof(text), "Waiting for %s", peer_.name);
        lv_label_set_text(hint_, text);
        if (ring_.t() < 1000) ring_.run(1000);
        break;
    case MESH_PROMPT_DONE:
        lv_label_set_text(title_, "Added!");
        lv_label_set_text(hint_, "");
        if (ring_.t() < 1000) ring_.run(1000);
        lv_anim_delete(this, set_pair);
        tween(set_pair, pair_, 1000, kPairMs, nullptr);
        break;
    default:
        break;
    }
    lv_obj_update_layout(root_);
    apply_layout();
}

void FriendScreen::update(const terminal_view_state_t &state) {
    if (leaving_ || state.friend_prompt == MESH_PROMPT_NONE) return;
    if (std::memcmp(id_, state.friend_prompt_id, sizeof(id_)) != 0 || shown_ == MESH_PROMPT_NONE) {
        // A different rider on a page already up: start over.
        if (landed_ && shown_ != MESH_PROMPT_NONE) ring_.run(0);
        accepted_ = false;
        pair_ = 0;
        set_prompt(state);
    }
    show_state(state.friend_prompt);
}
