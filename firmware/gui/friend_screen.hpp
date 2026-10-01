#pragma once

#include "gui_app.hpp"
#include "hold_ring.hpp"
#include "screen.hpp"

// Add friend: pair mode (docs/blueprints/04_rider_mesh.md U1-U5, design/screens/07-rider-mesh).
// gui_app.cpp decides when the page is up (double tap, an incoming request, the exits); this page
// draws the card and turns touches into holds.
//
//   searching  nobody in range yet: our character, a pulse in our colour, "Looking for riders..." (no
//              title)
//   card       the rider in the centre at 2x the 32px art with their name over them, the next rider
//              either side at 1x and faded. A drag follows the finger; letting go snaps to the
//              nearest rider on an S curve (0.3 s), a flick steps one. Tapping the left or right
//              edge steps one too (gloves).
//   hold       2 s anywhere fills the ring in the centre rider's colour, then asks them (or accepts,
//              if they are asking us): "Waiting for <name> to accept".
//   waiting    a rider asking us is snapped to the centre (never under a finger) and wiggles:
//              "<name> is waiting for you to accept".
//   done       "Added!", our character slides in beside theirs (the old prompt's pair animation).
//   no answer  our request lapsed: "<name> didn't answer" for 2 s, then the card again.
//
// The avatar buffers (3 x 3 KB) are allocated when the page enters and freed when it leaves: the
// card is a rare page and the C3 has no RAM to keep them for it.
class FriendScreen : public Screen {
public:
    FriendScreen();
    ~FriendScreen() override;
    void update(const terminal_view_state_t &state) override;
    void enter() override;
    void leave(LeaveDone done, void *ctx) override;
    bool entered() const override;

    void set_hold_handler(gui_friend_hold_cb_t cb) { hold_cb_ = cb; }

    // Taps here step the card, so a double tap here is not "close".
    bool in_step_zone(int32_t x) const { return x < width_ / 5 || x > width_ - width_ / 5; }

    // lv_tick of the last touch on the page, for gui_app's idle exit.
    uint32_t last_touch_ms() const { return last_touch_ms_; }

private:
    struct Item {
        uint8_t id[MESH_RIDER_ID_LEN];
        mesh_look_t look;
        bool waiting;
    };
    struct Slot {
        lv_obj_t *img = nullptr;
        uint8_t *buf = nullptr;   // 1x art; null while the page is down (or if the allocation failed)
        lv_image_dsc_t dsc{};
        bool used = false;
        uint8_t id[MESH_RIDER_ID_LEN] = {0};
        uint8_t avatar = 0;
        uint32_t accent = 0;
    };
    static constexpr int kSlots = 3;

    void build_items(const terminal_view_state_t &state);
    int find_item(const uint8_t id[MESH_RIDER_ID_LEN]) const;
    void follow_centre();
    void track_prompt(const terminal_view_state_t &state);
    void go_to(int index, bool wiggle);
    void assign_slots();
    void reconcile_ring(const terminal_view_state_t &state);
    void set_texts(const terminal_view_state_t &state);
    void set_searching(bool on);
    void apply_layout();
    void alloc_art();
    void free_art();
    bool ring_wants_full(const terminal_view_state_t &state) const;

    static void set_in(void *var, int32_t value);
    static void set_pair(void *var, int32_t value);
    static void set_fade(void *var, int32_t value);
    static void set_view(void *var, int32_t value);
    static void set_wiggle(void *var, int32_t value);
    static void set_pulse(void *var, int32_t value);
    static void set_reveal(void *var, int32_t value);
    static FriendScreen *from_anim(lv_anim_t *a);
    void tween(lv_anim_exec_xcb_t exec, int32_t from, int32_t to, uint32_t ms,
               lv_anim_completed_cb_t completed, lv_anim_path_cb_t path = lv_anim_path_ease_in_out);

    static void on_press(lv_event_t *e);
    static void on_pressing(lv_event_t *e);
    static void on_release(lv_event_t *e);
    static void on_short_click(lv_event_t *e);
    static void on_ring_full(void *ctx);

    int32_t px(float ref_240) const;
    static int32_t touch_x();

    float scale_ = 1.0f;
    int32_t width_ = 240;
    int32_t avatar_zoom_ = 2;  // centre: whole-number upscale of the 32px art; the sides are half
    gui_friend_hold_cb_t hold_cb_ = nullptr;

    lv_obj_t *title_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    lv_obj_t *self_img_ = nullptr;  // our character (gui_logo_src): searching, and the done pair
    lv_obj_t *pulse_ = nullptr;
    HoldRing ring_;
    Slot slots_[kSlots];
    uint8_t *art_ = nullptr;

    Item items_[VIEW_CANDIDATES_MAX];
    int n_ = 0;
    int centre_ = 0;               // index in items_ the card rests on
    bool has_centre_ = false;
    uint8_t centre_id_[MESH_RIDER_ID_LEN] = {0};
    int32_t view_ = 0;             // centre position x1000; continuous while the card moves

    bool pressed_ = false;
    bool dragging_ = false;
    bool moved_ = false;           // this press dragged: its click is not an edge tap
    int32_t press_x_ = 0;
    int32_t press_view_ = 0;
    int press_centre_ = 0;
    uint32_t last_touch_ms_ = 0;

    int32_t ring_target_ = 0;
    uint8_t ring_id_[MESH_RIDER_ID_LEN] = {0};  // whose hold the ring is showing
    uint8_t held_id_[MESH_RIDER_ID_LEN] = {0};
    uint32_t held_ms_ = 0;
    bool held_ = false;

    mesh_prompt_state_t prompt_ = MESH_PROMPT_NONE;
    uint8_t prompt_id_[MESH_RIDER_ID_LEN] = {0};
    char prompt_name_[MESH_NAME_MAX + 1] = {0};
    bool snap_pending_ = false;
    uint8_t snap_id_[MESH_RIDER_ID_LEN] = {0};
    bool wiggle_after_ = false;
    uint32_t no_answer_until_ = 0;
    bool no_answer_ = false;
    char no_answer_name_[MESH_NAME_MAX + 1] = {0};

    int32_t in_ = 0;      // 0..1000, content below and clear .. risen and opaque
    int32_t pair_ = 0;    // 0..1000, done: their character alone .. both side by side
    int32_t fade_ = 1000;
    int32_t wiggle_ = 0;  // 0..1000 through the wiggle; 0 and 1000 are at rest
    int32_t pulse_t_ = 0;
    int32_t reveal_ = 1000;  // 0..1000, searching .. the card, as riders first arrive
    bool searching_ = false;
    bool landed_ = false;
    bool leaving_ = false;
    LeaveDone done_ = nullptr;
    void *done_ctx_ = nullptr;
    char title_text_[MESH_NAME_MAX + 16] = {0};
    char hint_text_[2 * MESH_NAME_MAX + 40] = {0};
};
