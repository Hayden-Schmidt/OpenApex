#include "friend_screen.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "avatar_art.hpp"
#include "avatars_data.h"
#include "ease.hpp"
#include "gui_font.hpp"
#include "theme.hpp"

namespace {

// Geometry in a 240px frame, scaled at runtime. The title, character and hint sit where the old
// friend prompt (design/screens/06-add-friend) put them.
constexpr float kTitleCy = 78.0f;
constexpr float kAvatarCy = 127.0f;
constexpr float kHintCy = 180.0f;
constexpr float kHintW = 176.0f;         // two lines at most inside the round panel
constexpr float kStep = 78.0f;           // centre to side rider
constexpr float kPairGap = 8.0f;         // between the two characters when side by side
constexpr float kPulseMax = 200.0f;      // searching pulse, outer diameter at the end of a beat
constexpr float kPulseW = 3.0f;
constexpr float kTitleFontRef = 30.0f;
constexpr float kHintFontRef = 15.0f;
constexpr float kRingW = 5.0f;           // as the call page's
constexpr float kRise = 24.0f;           // entry rise
constexpr float kDragStart = 8.0f;       // a press that moves this far is a drag, not a hold
constexpr float kFlick = 30.0f;          // a drag this long steps one rider even if it did not get halfway
constexpr float kWiggleAmp = 6.0f;
constexpr int32_t kSideOpa = 90;         // the riders either side, out of 255

constexpr uint32_t kInMs = 500;
constexpr uint32_t kPairMs = 600;
constexpr uint32_t kFadeMs = 300;
constexpr uint32_t kStepMs = 300;        // one rider along, on an S curve
constexpr uint32_t kWiggleMs = 600;
constexpr uint32_t kPulseMs = 1600;
constexpr uint32_t kNoAnswerMs = 2000;
constexpr uint32_t kHeldGraceMs = 1500;  // a completed hold keeps the ring full until the mesh says WAITING

float lerp(float a, float b, float t) { return a + (b - a) * t; }

void place_centred(lv_obj_t *obj, int32_t cx, int32_t cy) {
    lv_obj_set_pos(obj, cx - lv_obj_get_width(obj) / 2, cy - lv_obj_get_height(obj) / 2);
}

bool id_eq(const uint8_t *a, const uint8_t *b) { return std::memcmp(a, b, MESH_RIDER_ID_LEN) == 0; }

bool tick_reached(uint32_t at) { return static_cast<int32_t>(lv_tick_get() - at) >= 0; }

void set_text(lv_obj_t *label, char *cache, size_t cap, const char *text) {
    if (std::strcmp(cache, text) == 0) return;
    std::snprintf(cache, cap, "%s", text);
    lv_label_set_text(label, cache);
}

} // namespace

int32_t FriendScreen::px(float ref_240) const {
    return static_cast<int32_t>(std::lround(ref_240 * scale_));
}

int32_t FriendScreen::touch_x() {
    lv_point_t p = {0, 0};
    lv_indev_t *indev = lv_indev_active();
    if (indev != nullptr) lv_indev_get_point(indev, &p);
    return p.x;
}

FriendScreen::FriendScreen() {
    width_ = lv_display_get_horizontal_resolution(lv_display_get_default());
    scale_ = static_cast<float>(width_) / 240.0f;
    avatar_zoom_ = static_cast<int32_t>(std::lround(64.0f * scale_ / AVATAR_ART_PX));
    if (avatar_zoom_ < 2) avatar_zoom_ = 2;

    lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(root_, false);
    lv_obj_add_event_cb(root_, on_press, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(root_, on_pressing, LV_EVENT_PRESSING, this);
    lv_obj_add_event_cb(root_, on_release, LV_EVENT_RELEASED, this);
    lv_obj_add_event_cb(root_, on_release, LV_EVENT_PRESS_LOST, this);
    lv_obj_add_event_cb(root_, on_short_click, LV_EVENT_SHORT_CLICKED, this);

    pulse_ = lv_obj_create(root_);
    lv_obj_remove_style_all(pulse_);
    lv_obj_set_style_radius(pulse_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(pulse_, px(kPulseW), 0);
    lv_obj_add_flag(pulse_, LV_OBJ_FLAG_HIDDEN);

    title_ = lv_label_create(root_);
    lv_obj_set_style_text_color(title_, lv_color_white(), 0);
    lv_obj_set_style_text_font(title_, montserrat_at_most(px(kTitleFontRef)), 0);
    lv_label_set_text(title_, "");

    hint_ = lv_label_create(root_);
    lv_obj_set_style_text_font(hint_, montserrat_at_most(px(kHintFontRef)), 0);
    lv_obj_set_style_text_align(hint_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(hint_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint_, px(kHintW));
    lv_label_set_text(hint_, "");

    // Nearest-neighbour zoom keeps the pixel art crisp; sizes are whole multiples except mid-motion.
    self_img_ = lv_image_create(root_);
    for (Slot &s : slots_) s.img = lv_image_create(root_);
    for (lv_obj_t *img : {self_img_, slots_[0].img, slots_[1].img, slots_[2].img}) {
        lv_image_set_antialias(img, false);
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
        lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    }
    lv_image_set_scale(self_img_, static_cast<uint32_t>(avatar_zoom_ * LV_SCALE_NONE));
    lv_obj_set_size(self_img_, AVATAR_ART_PX * avatar_zoom_, AVATAR_ART_PX * avatar_zoom_);

    ring_.create(root_, width_, px(kRingW));

    for (lv_obj_t *obj : {title_, hint_, self_img_, pulse_, slots_[0].img, slots_[1].img, slots_[2].img}) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    }
    apply_layout();
}

FriendScreen::~FriendScreen() { free_art(); }

// -- avatar buffers --------------------------------------------------------------------------------

void FriendScreen::alloc_art() {
    if (art_ != nullptr) return;
    constexpr int kBytes = avatar_art_bytes(AVATAR_ART_PX, AVATAR_ART_PX);
    art_ = static_cast<uint8_t *>(std::malloc(kSlots * kBytes));
    if (art_ == nullptr) {
        LV_LOG_WARN("Add friend: no RAM for avatars, showing names only");
        return;
    }
    for (int i = 0; i < kSlots; i++) slots_[i].buf = art_ + i * kBytes;
}

void FriendScreen::free_art() {
    for (Slot &s : slots_) {
        s.used = false;
        s.buf = nullptr;
        if (s.img != nullptr) lv_obj_add_flag(s.img, LV_OBJ_FLAG_HIDDEN);
    }
    std::free(art_);
    art_ = nullptr;
}

// -- animation plumbing ------------------------------------------------------------------------

FriendScreen *FriendScreen::from_anim(lv_anim_t *a) { return static_cast<FriendScreen *>(a->var); }

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

void FriendScreen::set_view(void *var, int32_t value) {
    auto *self = static_cast<FriendScreen *>(var);
    self->view_ = value;
    self->assign_slots();
    self->apply_layout();
}

void FriendScreen::set_wiggle(void *var, int32_t value) {
    auto *self = static_cast<FriendScreen *>(var);
    self->wiggle_ = value;
    self->apply_layout();
}

void FriendScreen::set_pulse(void *var, int32_t value) {
    auto *self = static_cast<FriendScreen *>(var);
    self->pulse_t_ = value;
    self->apply_layout();
}

// As CallScreen::tween: duration scales with the distance left (per 1000).
void FriendScreen::tween(lv_anim_exec_xcb_t exec, int32_t from, int32_t to, uint32_t ms,
                         lv_anim_completed_cb_t completed, lv_anim_path_cb_t path) {
    lv_anim_delete(this, exec);
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

// -- the card ----------------------------------------------------------------------------------

void FriendScreen::build_items(const terminal_view_state_t &state) {
    if (state.friend_prompt == MESH_PROMPT_DONE) {
        // Now friends: they are off the candidate list, the card holds just them.
        n_ = 1;
        std::memcpy(items_[0].id, state.friend_prompt_id, MESH_RIDER_ID_LEN);
        items_[0].look = state.friend_peer;
        items_[0].waiting = false;
        return;
    }
    n_ = state.friend_cand_count < VIEW_CANDIDATES_MAX ? state.friend_cand_count : VIEW_CANDIDATES_MAX;
    for (int i = 0; i < n_; i++) {
        std::memcpy(items_[i].id, state.friend_cands[i].id, MESH_RIDER_ID_LEN);
        items_[i].look = state.friend_cands[i].look;
        items_[i].waiting = state.friend_cands[i].waiting;
    }
}

int FriendScreen::find_item(const uint8_t id[MESH_RIDER_ID_LEN]) const {
    for (int i = 0; i < n_; i++) {
        if (id_eq(items_[i].id, id)) return i;
    }
    return -1;
}

// Keeps the card on the same rider when the list reorders or shrinks under it.
void FriendScreen::follow_centre() {
    if (n_ == 0) {
        has_centre_ = false;
        centre_ = 0;
        lv_anim_delete(this, set_view);
        view_ = 0;
        return;
    }
    const int k = has_centre_ ? find_item(centre_id_) : -1;
    if (k >= 0) {
        if (k != centre_) {
            // Same rider, new index: shift the view with them so nothing on screen jumps.
            const int32_t shift = (k - centre_) * 1000;
            const bool moving = lv_anim_get(this, set_view) != nullptr;
            view_ += shift;
            press_view_ += shift;
            centre_ = k;
            if (moving) go_to(k, wiggle_after_);
        }
        return;
    }
    if (!has_centre_) {
        centre_ = 0;
        view_ = 0;
        has_centre_ = true;
        std::memcpy(centre_id_, items_[0].id, MESH_RIDER_ID_LEN);
        return;
    }
    // The rider on the card left: slide to whoever is now nearest that spot.
    go_to(centre_ < n_ ? centre_ : n_ - 1, false);
}

void FriendScreen::go_to(int index, bool wiggle) {
    if (n_ == 0) return;
    index = index < 0 ? 0 : (index >= n_ ? n_ - 1 : index);
    centre_ = index;
    has_centre_ = true;
    std::memcpy(centre_id_, items_[index].id, MESH_RIDER_ID_LEN);
    wiggle_after_ = wiggle;
    const int32_t target = index * 1000;
    const lv_anim_completed_cb_t arrived = [](lv_anim_t *a) {
        FriendScreen *s = from_anim(a);
        if (!s->wiggle_after_) return;
        s->wiggle_after_ = false;
        s->tween(set_wiggle, 0, 1000, kWiggleMs, nullptr, lv_anim_path_linear);
    };
    if (view_ == target) {
        lv_anim_delete(this, set_view);
        if (wiggle) {
            wiggle_after_ = false;
            tween(set_wiggle, 0, 1000, kWiggleMs, nullptr, lv_anim_path_linear);
        }
        return;
    }
    // One rider along in kStepMs; tween() scales by the distance.
    tween(set_view, view_, target, kStepMs, arrived, anim_path<ease_in_out_cubic>);
}

// Prompt edges: a rider asking us is snapped to the centre and wiggles; our request lapsing shows
// "didn't answer"; done slides our character in.
void FriendScreen::track_prompt(const terminal_view_state_t &state) {
    const mesh_prompt_state_t st = state.friend_prompt;
    if (st != prompt_ || !id_eq(state.friend_prompt_id, prompt_id_)) {
        if (prompt_ == MESH_PROMPT_WAITING && st == MESH_PROMPT_NONE) {
            no_answer_ = true;
            no_answer_until_ = lv_tick_get() + kNoAnswerMs;
            std::memcpy(no_answer_name_, prompt_name_, sizeof(no_answer_name_));
        }
        snap_pending_ = st == MESH_PROMPT_OFFER;
        if (snap_pending_) std::memcpy(snap_id_, state.friend_prompt_id, MESH_RIDER_ID_LEN);
        if (st == MESH_PROMPT_DONE) {
            lv_image_set_src(self_img_, gui_logo_src());
            no_answer_ = false;
            lv_anim_delete(this, set_wiggle);
            wiggle_ = 0;
            tween(set_pair, pair_, 1000, kPairMs, nullptr);
        }
        if (st != MESH_PROMPT_NONE) held_ = false; // the mesh has taken over from our grace
        prompt_ = st;
        std::memcpy(prompt_id_, state.friend_prompt_id, MESH_RIDER_ID_LEN);
        std::snprintf(prompt_name_, sizeof(prompt_name_), "%s", state.friend_peer.name);
    }
    if (snap_pending_ && !pressed_) {
        const int k = find_item(snap_id_);
        if (k >= 0) {
            snap_pending_ = false;
            go_to(k, true);
        }
    }
    if (no_answer_ && tick_reached(no_answer_until_)) no_answer_ = false;
    if (held_ && lv_tick_elaps(held_ms_) >= kHeldGraceMs) held_ = false;
}

// Up to three riders get an avatar buffer: the ones nearest the centre of the view. Buffers follow
// the rider, not the index, so a reorder never redraws art.
void FriendScreen::assign_slots() {
    int want[kSlots];
    int nw = 0;
    float dist[kSlots];
    for (int i = 0; i < n_; i++) {
        const float d = std::fabs(static_cast<float>(i * 1000 - view_) / 1000.0f);
        if (d >= 1.5f) continue;
        if (nw < kSlots) {
            want[nw] = i;
            dist[nw++] = d;
            continue;
        }
        int far = 0;
        for (int j = 1; j < kSlots; j++) far = dist[j] > dist[far] ? j : far;
        if (d < dist[far]) {
            want[far] = i;
            dist[far] = d;
        }
    }
    for (Slot &s : slots_) {
        if (!s.used) continue;
        bool keep = false;
        for (int j = 0; j < nw && !keep; j++) keep = id_eq(items_[want[j]].id, s.id);
        if (!keep) {
            s.used = false;
            lv_obj_add_flag(s.img, LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (int j = 0; j < nw; j++) {
        const Item &it = items_[want[j]];
        Slot *slot = nullptr;
        for (Slot &s : slots_) {
            if (s.used && id_eq(s.id, it.id)) slot = &s;
        }
        if (slot == nullptr) {
            for (Slot &s : slots_) {
                if (!s.used && slot == nullptr) slot = &s;
            }
            if (slot == nullptr) continue;
            slot->used = true;
            std::memcpy(slot->id, it.id, MESH_RIDER_ID_LEN);
            slot->avatar = static_cast<uint8_t>(~it.look.avatar); // force a draw below
        }
        if (slot->buf == nullptr) continue;
        if (slot->avatar != it.look.avatar || slot->accent != it.look.accent_rgb) {
            slot->avatar = it.look.avatar;
            slot->accent = it.look.accent_rgb;
            avatar_art_draw(slot->buf, AVATAR_ART_PX, it.look.avatar, nullptr, lv_color_hex(it.look.accent_rgb));
            avatar_art_describe(&slot->dsc, slot->buf, AVATAR_ART_PX, AVATAR_ART_PX);
            lv_image_set_src(slot->img, &slot->dsc);
            lv_obj_invalidate(slot->img);
        }
    }
}

bool FriendScreen::ring_wants_full(const terminal_view_state_t &state) const {
    (void)state;
    if (n_ == 0) return false;
    const uint8_t *c = items_[centre_].id;
    if ((prompt_ == MESH_PROMPT_WAITING || prompt_ == MESH_PROMPT_DONE) && id_eq(prompt_id_, c)) return true;
    return held_ && id_eq(held_id_, c);
}

// At rest the ring shows the handshake: full in the centre rider's colour once we have held on them,
// empty otherwise. A finger on the panel drives it instead.
void FriendScreen::reconcile_ring(const terminal_view_state_t &state) {
    if (pressed_ && !dragging_) return;
    const int32_t want = ring_wants_full(state) && !dragging_ ? 1000 : 0;
    if (want == ring_target_) return;
    if (want == 1000) {
        ring_.set_color(lv_color_hex(items_[centre_].look.accent_rgb));
        std::memcpy(ring_id_, items_[centre_].id, MESH_RIDER_ID_LEN);
    }
    ring_target_ = want;
    ring_.run(want);
}

void FriendScreen::set_texts(const terminal_view_state_t &state) {
    char hint[sizeof(hint_text_)];
    lv_color_t colour = gui_theme().palette();
    if (state.friend_prompt == MESH_PROMPT_DONE) {
        set_text(title_, title_text_, sizeof(title_text_), "Added!");
        hint[0] = '\0';
    } else if (n_ == 0) {
        set_text(title_, title_text_, sizeof(title_text_), "Add friend");
        if (no_answer_) {
            std::snprintf(hint, sizeof(hint), "%s didn't answer", no_answer_name_);
        } else {
            std::snprintf(hint, sizeof(hint), "Looking for riders...");
        }
    } else {
        const Item &c = items_[centre_];
        colour = lv_color_hex(c.look.accent_rgb);
        set_text(title_, title_text_, sizeof(title_text_), c.look.name);
        if (no_answer_ && std::strcmp(no_answer_name_, c.look.name) == 0) {
            std::snprintf(hint, sizeof(hint), "%s didn't answer", c.look.name);
        } else if (prompt_ == MESH_PROMPT_WAITING && id_eq(prompt_id_, c.id)) {
            std::snprintf(hint, sizeof(hint), "Waiting for %s to accept", c.look.name);
        } else if (c.waiting) {
            std::snprintf(hint, sizeof(hint), "%s is waiting for you to accept", c.look.name);
        } else {
            std::snprintf(hint, sizeof(hint), "Hold to add");
        }
    }
    set_text(hint_, hint_text_, sizeof(hint_text_), hint);
    lv_obj_set_style_text_color(hint_, colour, 0);
}

void FriendScreen::set_searching(bool on) {
    if (on == searching_) return;
    searching_ = on;
    if (!on) {
        lv_anim_delete(this, set_pulse);
        pulse_t_ = 0;
        return;
    }
    lv_image_set_src(self_img_, gui_logo_src());
    lv_obj_set_style_border_color(pulse_, gui_theme().palette(), 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, this);
    lv_anim_set_exec_cb(&a, set_pulse);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, kPulseMs);
    lv_anim_set_path_cb(&a, anim_path<ease_out_expo>);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

void FriendScreen::apply_layout() {
    const float in = static_cast<float>(in_) / 1000.0f;
    const float pair = static_cast<float>(pair_) / 1000.0f;
    const int32_t dy = px(kRise * (1.0f - in));
    const int32_t page = 255 * in_ / 1000 * fade_ / 1000;
    const int32_t cx = width_ / 2;
    const int32_t acy = px(kAvatarCy) + dy;
    const bool done = prompt_ == MESH_PROMPT_DONE;

    place_centred(title_, cx, px(kTitleCy) + dy);
    place_centred(hint_, cx, px(kHintCy) + dy);
    for (lv_obj_t *obj : {title_, hint_}) lv_obj_set_style_opa(obj, static_cast<lv_opa_t>(page), 0);
    lv_obj_set_style_opa(ring_.obj(), static_cast<lv_opa_t>(page), 0);

    // Searching: our character with a pulse in our colour beating out from it.
    const int32_t centre_px = AVATAR_ART_PX * avatar_zoom_;
    if (searching_) {
        const float t = static_cast<float>(pulse_t_) / 1000.0f;
        const int32_t d = static_cast<int32_t>(std::lround(lerp(static_cast<float>(centre_px), px(kPulseMax), t)));
        lv_obj_set_size(pulse_, d, d);
        place_centred(pulse_, cx, acy);
        lv_obj_set_style_border_opa(pulse_, static_cast<lv_opa_t>(page * (1.0f - t) * 0.8f), 0);
        lv_obj_remove_flag(pulse_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(pulse_, LV_OBJ_FLAG_HIDDEN);
    }

    // Done: theirs moves right, ours slides in on the left.
    const float half = (static_cast<float>(centre_px) + px(kPairGap)) / 2.0f;
    if (searching_ || done) {
        const int32_t sx = searching_ ? cx : static_cast<int32_t>(std::lround(lerp(cx, cx - half, pair)));
        place_centred(self_img_, sx, acy);
        lv_obj_set_style_opa(self_img_, static_cast<lv_opa_t>(searching_ ? page : page * pair_ / 1000), 0);
        lv_obj_remove_flag(self_img_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(self_img_, LV_OBJ_FLAG_HIDDEN);
    }

    const float wt = static_cast<float>(wiggle_) / 1000.0f;
    const float wiggle_dx = wiggle_ > 0 && wiggle_ < 1000
                                ? px(kWiggleAmp) * std::sin(wt * 3.0f * 2.0f * 3.14159265f) * (1.0f - wt)
                                : 0.0f;
    for (Slot &s : slots_) {
        const int k = s.used && s.buf != nullptr ? find_item(s.id) : -1;
        const float d = k < 0 ? 9.0f : static_cast<float>(k * 1000 - view_) / 1000.0f;
        const float ad = std::fabs(d);
        if (ad >= 1.5f) {
            lv_obj_add_flag(s.img, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const float z = lerp(static_cast<float>(avatar_zoom_), avatar_zoom_ / 2.0f, ad < 1.0f ? ad : 1.0f);
        const int32_t size = static_cast<int32_t>(std::lround(AVATAR_ART_PX * z));
        lv_image_set_scale(s.img, static_cast<uint32_t>(std::lround(z * LV_SCALE_NONE)));
        lv_obj_set_size(s.img, size, size);
        float x = cx + d * px(kStep);
        if (ad < 0.5f) x += wiggle_dx;
        if (done) x += half * pair;
        const float o = ad <= 1.0f ? lerp(255.0f, kSideOpa, ad) : kSideOpa * (1.0f - (ad - 1.0f) * 2.0f);
        place_centred(s.img, static_cast<int32_t>(std::lround(x)), acy);
        lv_obj_set_style_opa(s.img, static_cast<lv_opa_t>(o * page / 255.0f), 0);
        lv_obj_remove_flag(s.img, LV_OBJ_FLAG_HIDDEN);
    }
}

void FriendScreen::update(const terminal_view_state_t &state) {
    if (leaving_) return;
    build_items(state);
    follow_centre();
    track_prompt(state);
    set_searching(n_ == 0 && state.friend_prompt != MESH_PROMPT_DONE);
    assign_slots();
    reconcile_ring(state);
    set_texts(state);
    apply_layout();
}

// -- touch -------------------------------------------------------------------------------------

void FriendScreen::on_press(lv_event_t *e) {
    auto *self = static_cast<FriendScreen *>(lv_event_get_user_data(e));
    self->last_touch_ms_ = lv_tick_get();
    if (self->leaving_) return;
    self->pressed_ = true;
    self->dragging_ = false;
    self->moved_ = false;
    self->press_x_ = touch_x();
    self->press_view_ = self->view_;
    self->press_centre_ = self->centre_;
    // A hold needs a rider settled in the centre who we have not already asked.
    if (!self->landed_ || self->n_ == 0 || self->prompt_ == MESH_PROMPT_DONE) return;
    if (lv_anim_get(self, set_view) != nullptr) return;
    const uint8_t *c = self->items_[self->centre_].id;
    if ((self->prompt_ == MESH_PROMPT_WAITING && id_eq(self->prompt_id_, c)) || (self->held_ && id_eq(self->held_id_, c))) {
        return;
    }
    self->ring_.set_color(lv_color_hex(self->items_[self->centre_].look.accent_rgb));
    std::memcpy(self->ring_id_, c, MESH_RIDER_ID_LEN);
    self->ring_target_ = 1000;
    self->ring_.run(1000, on_ring_full, self);
}

void FriendScreen::on_ring_full(void *ctx) {
    auto *self = static_cast<FriendScreen *>(ctx);
    if (!self->pressed_ || self->dragging_) return;
    if (self->hold_cb_ != nullptr) self->hold_cb_(self->ring_id_);
    self->held_ = true;
    std::memcpy(self->held_id_, self->ring_id_, MESH_RIDER_ID_LEN);
    self->held_ms_ = lv_tick_get();
}

void FriendScreen::on_pressing(lv_event_t *e) {
    auto *self = static_cast<FriendScreen *>(lv_event_get_user_data(e));
    if (!self->pressed_ || self->n_ == 0 || self->prompt_ == MESH_PROMPT_DONE) return;
    const int32_t x = touch_x();
    if (!self->dragging_) {
        if (std::abs(x - self->press_x_) <= self->px(kDragStart)) return;
        self->dragging_ = true;
        self->moved_ = true;
        self->snap_pending_ = false; // the rider is choosing; a waiting rider stays first in the list
        lv_anim_delete(self, set_view);
        lv_anim_delete(self, set_wiggle);
        self->wiggle_ = 0;
        self->press_x_ = x;
        self->press_view_ = self->view_;
        self->press_centre_ = self->centre_;
    }
    // The card follows the finger, with a little give past either end.
    int32_t v = self->press_view_ - (x - self->press_x_) * 1000 / self->px(kStep);
    const int32_t lo = -250, hi = (self->n_ - 1) * 1000 + 250;
    v = v < lo ? lo : (v > hi ? hi : v);
    self->view_ = v;
    int nearest = static_cast<int>((v + 500) / 1000);
    nearest = nearest < 0 ? 0 : (nearest >= self->n_ ? self->n_ - 1 : nearest);
    self->centre_ = nearest;
    std::memcpy(self->centre_id_, self->items_[nearest].id, MESH_RIDER_ID_LEN);
    self->assign_slots();
    self->apply_layout();
}

void FriendScreen::on_release(lv_event_t *e) {
    auto *self = static_cast<FriendScreen *>(lv_event_get_user_data(e));
    if (!self->pressed_) return;
    self->pressed_ = false;
    self->last_touch_ms_ = lv_tick_get();
    if (!self->dragging_) return; // an unfinished hold: reconcile_ring traces it back
    self->dragging_ = false;
    const int32_t dx = touch_x() - self->press_x_;
    int target = static_cast<int>((self->view_ + 500 + 1000) / 1000) - 1; // round, also below zero
    if (target == self->press_centre_) {
        if (dx < -self->px(kFlick)) target++;
        if (dx > self->px(kFlick)) target--;
    }
    self->go_to(target, false);
}

void FriendScreen::on_short_click(lv_event_t *e) {
    auto *self = static_cast<FriendScreen *>(lv_event_get_user_data(e));
    if (self->moved_ || self->leaving_ || self->n_ < 2 || self->prompt_ == MESH_PROMPT_DONE) return;
    const int32_t x = touch_x();
    if (x < self->width_ / 5) self->go_to(self->centre_ - 1, false);
    if (x > self->width_ - self->width_ / 5) self->go_to(self->centre_ + 1, false);
}

// -- page --------------------------------------------------------------------------------------

void FriendScreen::enter() {
    lv_anim_delete(this, nullptr);
    alloc_art();
    for (Slot &s : slots_) {
        s.used = false;
        lv_obj_add_flag(s.img, LV_OBJ_FLAG_HIDDEN);
    }
    leaving_ = false;
    landed_ = false;
    pressed_ = dragging_ = moved_ = false;
    n_ = 0;
    has_centre_ = false;
    centre_ = 0;
    view_ = 0;
    held_ = false;
    no_answer_ = false;
    snap_pending_ = false;
    wiggle_after_ = false;
    prompt_ = MESH_PROMPT_NONE; // so a prompt already up reads as an edge (snap and wiggle)
    std::memset(prompt_id_, 0, sizeof(prompt_id_));
    in_ = 0;
    pair_ = 0;
    fade_ = 1000;
    wiggle_ = 0;
    searching_ = false;
    pulse_t_ = 0;
    ring_.reset();
    ring_target_ = 0;
    last_touch_ms_ = lv_tick_get();
    title_text_[0] = hint_text_[0] = '\0';
    lv_label_set_text(title_, "");
    lv_label_set_text(hint_, "");
    apply_layout();
    tween(set_in, 0, 1000, kInMs, [](lv_anim_t *a) { from_anim(a)->landed_ = true; }, anim_path<ease_out_expo>);
}

bool FriendScreen::entered() const { return landed_; }

// An unfinished ring traces back, then the page fades out. Done keeps its full ring to the end.
void FriendScreen::leave(LeaveDone done, void *ctx) {
    lv_anim_delete(this, set_in);
    lv_anim_delete(this, set_pair);
    lv_anim_delete(this, set_view);
    lv_anim_delete(this, set_wiggle);
    leaving_ = true;
    pressed_ = false;
    done_ = done;
    done_ctx_ = ctx;
    const HoldRing::Done fade = [](void *c) {
        auto *s = static_cast<FriendScreen *>(c);
        s->tween(set_fade, s->fade_, 0, kFadeMs, [](lv_anim_t *a) {
            FriendScreen *s2 = from_anim(a);
            s2->set_searching(false);
            s2->free_art();
            s2->done_(s2->done_ctx_);
        });
    };
    if (prompt_ == MESH_PROMPT_DONE) {
        fade(this);
    } else {
        ring_target_ = 0;
        ring_.run(0, fade, this);
    }
}
