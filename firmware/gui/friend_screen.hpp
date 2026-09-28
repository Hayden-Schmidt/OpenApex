#pragma once

#include "gui_app.hpp"
#include "hold_ring.hpp"
#include "screen.hpp"

// design reference/7. Add Freind Screen (a loose design: layout and motion here take licence).
//
// A rider nearby who is not a friend yet (terminal_view_state_t::friend_prompt, docs §5.7):
//   OFFER    their name over their character, "Hold to Accept" in their accent. A 2s hold anywhere
//            fills the HoldRing, our accent on the left fading through black to theirs on the right.
//   WAITING  we held, they have not: the ring stays full and the line reads "Waiting for <name>".
//   DONE     "Added!", and our character slides in beside theirs.
// mesh_core drops the prompt when it times out, is declined or is done, and this page leaves.
//
// Motion: the name and character rise in and fade up; leaving traces the ring back (unless done)
// and fades the page out.
class FriendScreen : public Screen {
public:
    FriendScreen();
    void update(const terminal_view_state_t &state) override;
    void enter() override;
    void leave(LeaveDone done, void *ctx) override;
    bool entered() const override;
    void set_accept_handler(gui_friend_accept_cb_t cb);

private:
    void apply_layout();
    void set_prompt(const terminal_view_state_t &state);
    void show_state(mesh_prompt_state_t st);
    static void set_in(void *var, int32_t value);
    static void set_pair(void *var, int32_t value);
    static void set_fade(void *var, int32_t value);
    static FriendScreen *from_anim(lv_anim_t *a);
    void tween(lv_anim_exec_xcb_t exec, int32_t from, int32_t to, uint32_t ms,
               lv_anim_completed_cb_t completed, lv_anim_path_cb_t path = lv_anim_path_ease_in_out);

    static void on_press(lv_event_t *e);
    static void on_release(lv_event_t *e);
    void start_hold();

    int32_t px(float ref_240) const;

    float scale_ = 1.0f;
    int32_t width_ = 240;
    int32_t avatar_zoom_ = 2;  // whole-number upscale of the 32px art
    gui_friend_accept_cb_t accept_cb_ = nullptr;

    lv_obj_t *title_ = nullptr;   // their name, then "Added!"
    lv_obj_t *peer_img_ = nullptr;
    lv_obj_t *self_img_ = nullptr;
    lv_obj_t *hint_ = nullptr;    // "Hold to Accept" / "Waiting for <name>"
    HoldRing ring_;

    // Their character and ours at 1x (AVATAR_ART_PX); lv_image zooms them.
    uint8_t peer_buf_[32 * 32 * 3];
    uint8_t self_buf_[32 * 32 * 3];
    lv_image_dsc_t peer_dsc_{};
    lv_image_dsc_t self_dsc_{};

    int32_t in_ = 0;    // 0..1000, content below and clear .. risen and opaque
    int32_t pair_ = 0;  // 0..1000, their character alone and centred .. both side by side
    int32_t fade_ = 1000;
    bool landed_ = false;
    bool leaving_ = false;
    bool pressed_ = false;
    bool accepted_ = false;
    mesh_prompt_state_t shown_ = MESH_PROMPT_NONE;
    uint8_t id_[MESH_RIDER_ID_LEN] = {0};
    mesh_look_t peer_{};
    mesh_look_t self_{};
    LeaveDone done_ = nullptr;
    void *done_ctx_ = nullptr;
};
