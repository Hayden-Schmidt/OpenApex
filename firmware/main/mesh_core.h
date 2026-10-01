#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mesh_proto.h"
#include "mesh_transport.h" // MESH_RSSI_UNKNOWN

#ifdef __cplusplus
extern "C" {
#endif

// Rider-mesh state machine (docs/blueprints/04_rider_mesh.md, Phase 1): the peer table, the friend
// list, the menu-driven friending handshake, the friend prompt, look pushes and position sharing.
// No radio, no clock, no NVS -- the caller passes frames in, time in, and a send callback out -- so
// the whole protocol runs in host tests. mesh.c owns the only instance on the terminal and
// serialises every call with a mutex.

#define MESH_MAX_PEERS 8u
#define MESH_MAX_FRIENDS 16u
#define MESH_HELLO_PERIOD_MS 2000u
#define MESH_POS_PERIOD_MS 1000u
#define MESH_PEER_TIMEOUT_MS 10000u   // no frame for this long: out of range
#define MESH_OWN_POS_MAX_AGE_MS 5000u // our own fix older than this is neither shared nor compared
#define MESH_OFFSET_MAX_AGE_MS 5000u  // a friend's offset older than this restarts smoothing
// The compass keeps a friend at their last bearing this long without a fresh offset (lost frames, a
// stalled phone fix), so a marker never flickers off and on. Out of range still ends it sooner:
// a friend not heard from at all for MESH_PEER_TIMEOUT_MS leaves the peer table.
#define MESH_OFFSET_SHOW_MS 30000u
#define MESH_OWN_FIXES 6u             // own-fix history used to align a friend's fix in time
#define MESH_OFFSET_TAU_S 3.0f        // time constant of the east/north offset smoothing
#define MESH_PROMPT_TIMEOUT_MS 20000u // time *on screen* before an unanswered request lapses
#define MESH_PROMPT_DONE_MS 3000u     // "now friends" confirmation before the page closes
#define MESH_REQUEST_RETRY_MS 2000u
#define MESH_ACCEPT_RETRY_MS 1000u
// A reply is never sent the moment its trigger arrives: the sender is still advertising that
// frame for up to 300 ms (FRAME_WINDOW_MS, mesh_transport_ble.c), and a C3 hears little while its
// own advert is on air. Replies wait past that window, and retries and periodic frames (HELLO, POS)
// get jitter so two terminals on the same period cannot stay phase-locked onto each other's adverts.
#define MESH_REPLY_DELAY_MS 400u
#define MESH_RETRY_JITTER_MS 400u
#define MESH_ACCEPT_TRIES 5u
// The requester commits on ACCEPT and then resends KEYS until the ACK. It keeps going longer than the
// responder's ACCEPT retries, and the responder still takes KEYS from a rider it consented to after
// its own retries end (`consented`), so a burst of lost frames cannot leave one side a friend and the
// other not.
#define MESH_KEYS_TRIES 12u
#define MESH_LOOK_PUSH_MS 1000u
#define MESH_LOOK_PUSH_TRIES 3u
// Pair mode (U1-U5): the card lists riders also in Add friend, nearest first. Only riders whose
// smoothed signal is at least MESH_PAIR_MIN_RSSI dBm (bikes brought together) are listed, and the
// order changes only when one rider is clearly nearer than the one ahead of it, so the card does not
// reshuffle on every RSSI wobble. The threshold is a starting value to tune on hardware.
#define MESH_PAIR_MIN_RSSI (-70)
#define MESH_PAIR_REORDER_DB 6
#define MESH_RSSI_ALPHA 0.3f
#define MESH_FRIEND_EVENTS 2u // the phone re-reads the whole list on connect, so a lost event is harmless

// Compile-time until the phone setting lands (Phase 3): the look is public in HELLO at all times.
#ifndef MESH_ALWAYS_DISCOVERABLE
#define MESH_ALWAYS_DISCOVERABLE 0
#endif

// Prompt gate: Add friend (and a prompt in it) may cover the nav page only when it cannot hide a turn
// the rider needs: not navigating, or the next maneuver more than 50 m out. Speed does not matter;
// friending on a straight is fine (owner, 2026-10-01).
#define MESH_PROMPT_MIN_TURN_M 50u

typedef void (*mesh_send_fn)(const uint8_t *frame, size_t len, void *ctx);

// Where a peer stands in the friending handshake (04.1.1, 04.1.4 POC form):
//   requester:  REQ_SENT --ACCEPT--> KEYS_SENT --ACK--> NONE
//   responder:  REQ_RECV --rider consents--> ACCEPT_SENT --KEYS--> NONE
typedef enum {
    MESH_HS_NONE = 0,
    MESH_HS_REQ_SENT,    // we asked (menu); retrying FRIEND_REQUEST
    MESH_HS_REQ_RECV,    // they asked; the prompt waits for the rider
    MESH_HS_ACCEPT_SENT, // we consented; retrying FRIEND_ACCEPT until their KEYS arrive
    MESH_HS_KEYS_SENT,   // we hold their key; retrying FRIEND_KEYS until their ACK arrives
} mesh_hs_t;

#define MESH_NO_FRIEND 0xFFu

typedef struct {
    bool used;
    uint8_t id[MESH_RIDER_ID_LEN]; // the peer's pseudonym as heard on air
    uint32_t heard_ms;             // last frame of any kind
    bool has_look;                 // `look` is valid: their public look, or a friend's LOOK
    bool public_look;              // their latest HELLO carried a look: they are in Add friend
    mesh_look_t look;
    bool has_rssi;
    float rssi_dbm;                // smoothed over the frames heard from them
    // Friend link, resolved from the pseudonym and cached until the friend list changes.
    uint8_t friend_idx;            // index into friends[], or MESH_NO_FRIEND
    uint32_t resolved_gen;
    bool resolved;
    bool announced;                // we already triggered a look push for this appearance
    // Position: offset from us to them at the same instant, smoothed (04.1.6).
    bool has_off;
    float off_east_m, off_north_m;
    uint32_t off_ms;
    // Handshake.
    uint8_t hs;                    // mesh_hs_t
    bool consented;                // our rider held on them (or asked them); their KEYS is welcome
    uint8_t tries;
    uint32_t next_try_ms;          // when the next handshake frame is due
    bool ack_due;                  // a FRIEND_ACK is owed at ack_at_ms
    uint32_t ack_at_ms;
} mesh_peer_t;

typedef enum {
    MESH_PROMPT_NONE = 0,
    MESH_PROMPT_OFFER,      // hold to friend (they asked)
    MESH_PROMPT_WAITING,    // we asked or consented; waiting on the other terminal
    MESH_PROMPT_DONE,       // now friends
} mesh_prompt_state_t;

typedef enum {
    MESH_FRIEND_EVENT_ADDED = 1,   // a friendship completed on this terminal
    MESH_FRIEND_EVENT_UPDATED = 2, // a friend's look or key changed
} mesh_friend_event_kind_t;

typedef struct {
    uint8_t kind;           // mesh_friend_event_kind_t
    mesh_friend_t record;
} mesh_friend_event_t;

typedef struct {
    int32_t lat_e7, lon_e7;
    uint32_t t_ms;
} mesh_fix_t;

typedef struct {
    bool enabled;
    bool has_identity;
    mesh_identity_t self;
    uint8_t self_tag_key[MESH_KEY_LEN];
    uint8_t prand[4];                       // drawn from the boot seed; the pseudonym is stable until reboot
    uint8_t pseud[MESH_RIDER_ID_LEN];       // what goes in every frame's sender field
    mesh_look_t look;                       // our own; name comes from `self`
    uint32_t session;
    uint16_t seq;
    uint32_t rng;                           // retry jitter only; not security-relevant

    bool always_discoverable;
    bool menu_open;                         // in Add friend (U1): our look is public
    int8_t pair_min_rssi;
    // Pair-mode card order: pseudonyms, nearest first, kept between calls for hysteresis.
    uint8_t cand[MESH_MAX_PEERS][MESH_RIDER_ID_LEN];
    uint8_t cand_n;

    bool has_own_pos;
    mesh_pos_t own_pos;
    uint32_t own_pos_ms;
    mesh_fix_t fixes[MESH_OWN_FIXES];       // newest last
    uint8_t fix_count;

    mesh_peer_t peers[MESH_MAX_PEERS];
    mesh_friend_t friends[MESH_MAX_FRIENDS];
    uint8_t tag_keys[MESH_MAX_FRIENDS][MESH_KEY_LEN];
    size_t friend_count;
    uint32_t friends_generation; // bumped on every friend-list change, for the NVS saver

    mesh_prompt_state_t prompt;
    uint8_t prompt_id[MESH_RIDER_ID_LEN];
    mesh_look_t prompt_look;
    uint32_t prompt_shown_ms;    // accumulated while the gate lets it show
    uint32_t prompt_done_at_ms;
    bool prompt_visible;

    uint8_t look_push_left;      // LOOK sends still owed to friends in range
    uint32_t last_look_ms;

    uint32_t last_tick_ms;
    bool hello_started;
    uint32_t next_hello_ms;      // periodic frames are jittered too (MESH_RETRY_JITTER_MS)
    uint32_t next_pos_ms;
    bool ticked;

    // Last position a friend sent, decrypted (development: `pos follow` places a phoneless board by it).
    bool has_friend_fix;
    mesh_pos_t friend_fix;

    mesh_friend_event_t events[MESH_FRIEND_EVENTS];
    size_t event_head, event_count;
} mesh_core_t;

void mesh_core_init(mesh_core_t *c, uint32_t session_seed);
void mesh_core_set_enabled(mesh_core_t *c, bool enabled);
void mesh_core_set_identity(mesh_core_t *c, const mesh_identity_t *id); // NULL: none, mesh idles
void mesh_core_set_look(mesh_core_t *c, const mesh_look_t *look);
void mesh_core_set_own_pos(mesh_core_t *c, const mesh_pos_t *pos, uint32_t now_ms);

// Add friend is open (U1): our look is public only while it is. Leaving it withdraws our unanswered
// request and passes over a rider asking us; a handshake whose keys are already moving completes.
void mesh_core_set_menu_open(mesh_core_t *c, bool open);
void mesh_core_set_always_discoverable(mesh_core_t *c, bool on);
void mesh_core_set_pair_min_rssi(mesh_core_t *c, int8_t dbm);

// Friend list (phone sync and NVS load). Upsert fails only when the list is full.
bool mesh_core_friend_upsert(mesh_core_t *c, const mesh_friend_t *f);
bool mesh_core_friend_remove(mesh_core_t *c, const uint8_t id[MESH_RIDER_ID_LEN]);
void mesh_core_friend_clear(mesh_core_t *c);
const mesh_friend_t *mesh_core_friend_find(const mesh_core_t *c, const uint8_t id[MESH_RIDER_ID_LEN]);

// Next friend event for the phone, oldest first. False when none are queued.
bool mesh_core_take_friend_event(mesh_core_t *c, mesh_friend_event_t *out);

// `rssi` is the frame's signal strength in dBm, or MESH_RSSI_UNKNOWN.
void mesh_core_rx(mesh_core_t *c, const uint8_t *frame, size_t len, int8_t rssi, uint32_t now_ms,
                  mesh_send_fn send, void *ctx);
// `prompt_allowed` is mesh_core_prompt_gate() for the current nav view.
void mesh_core_tick(mesh_core_t *c, uint32_t now_ms, bool prompt_allowed, mesh_send_fn send, void *ctx);

// The pair card (U2): riders who are not friends yet and are either in Add friend and close enough,
// or already asking us (waiting riders come first, whatever their signal). Nearest first, with
// hysteresis. Pointers are into the peer table; valid until the next call into the core.
size_t mesh_core_candidates(mesh_core_t *c, uint32_t now_ms, const mesh_peer_t **out, size_t cap);

// True when `p` is asking us to be friends (U4, "Ann is waiting").
bool mesh_core_peer_waiting(const mesh_peer_t *p);

// The rider held the ring on `pseudonym`'s card (U3/U4): accepts if they are asking us, otherwise
// asks them, replacing any request of ours still unanswered. False if nothing could be sent.
bool mesh_core_hold(mesh_core_t *c, const uint8_t pseudonym[MESH_RIDER_ID_LEN], uint32_t now_ms, mesh_send_fn send,
                    void *ctx);

// The two halves of mesh_core_hold. `request` fails if the slot is busy with another rider, the
// list is full, or the peer is unknown or already a friend.
bool mesh_core_request(mesh_core_t *c, const uint8_t pseudonym[MESH_RIDER_ID_LEN], uint32_t now_ms,
                       mesh_send_fn send, void *ctx);
void mesh_core_accept(mesh_core_t *c, const uint8_t pseudonym[MESH_RIDER_ID_LEN], uint32_t now_ms,
                      mesh_send_fn send, void *ctx);

bool mesh_core_prompt_gate(bool navigating, uint32_t turn_distance_m);

// The prompt to draw right now, if any (NONE while the gate holds it back).
mesh_prompt_state_t mesh_core_prompt(const mesh_core_t *c, uint8_t id[MESH_RIDER_ID_LEN], mesh_look_t *look);

typedef struct {
    const mesh_friend_t *f;
    float bearing_deg;      // true bearing from us, 0 = north
    float dist_m;
} mesh_friend_fix_t;

// Friends in range with a fresh, smoothed offset. Returns the count.
size_t mesh_core_friends_in_range(const mesh_core_t *c, uint32_t now_ms, mesh_friend_fix_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
