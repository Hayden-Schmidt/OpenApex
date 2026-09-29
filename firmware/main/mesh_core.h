#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mesh_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

// Rider-mesh state machine (docs/reference/rider_mesh_v1.md): the peer table, the friend list, the
// friending handshake, the friend prompt and position sharing. No radio, no clock, no NVS -- the
// caller passes frames in, time in, and a send callback out -- so the whole protocol runs in host
// tests. mesh.c owns the only instance on the terminal and serialises every call with a mutex.

#define MESH_MAX_PEERS 8u
#define MESH_MAX_FRIENDS 16u
#define MESH_MAX_SUPPRESSED 16u
#define MESH_HELLO_PERIOD_MS 2000u
#define MESH_POS_PERIOD_MS 1000u
#define MESH_PEER_TIMEOUT_MS 10000u   // no HELLO/POS for this long: out of range
#define MESH_OWN_POS_MAX_AGE_MS 5000u // our own fix older than this is not shared
#define MESH_PROMPT_TIMEOUT_MS 20000u // time *on screen* before an unanswered prompt lapses
#define MESH_PROMPT_DONE_MS 3000u     // "now friends" confirmation before the page closes
#define MESH_ACCEPT_RETRY_MS 1000u
#define MESH_ACCEPT_TRIES 5u
#define MESH_FRIEND_EVENTS 4u

// Prompt gate: a friend prompt may cover the nav page only when it cannot hide a turn the rider
// needs: slow (under 20 km/h), or the next maneuver more than 50 m out, or not navigating.
#define MESH_PROMPT_MAX_SPEED_KMH_X10 200u
#define MESH_PROMPT_MIN_TURN_M 50u

typedef void (*mesh_send_fn)(const uint8_t *frame, size_t len, void *ctx);

typedef struct {
    bool used;
    uint8_t id[MESH_RIDER_ID_LEN];
    mesh_look_t look;
    uint32_t heard_ms;      // last HELLO or POS
    bool has_pos;
    mesh_pos_t pos;
    uint32_t pos_ms;
    // Handshake, for a non-friend.
    bool local_accepted;    // our hold ring filled for this rider
    bool remote_accepted;   // their FRIEND_ACCEPT arrived; `remote_key` is valid
    uint8_t remote_key[MESH_KEY_LEN];
    bool acked;             // they acknowledged our accept: stop retrying
    uint8_t tries;
    uint32_t last_try_ms;
    bool replied;           // answered a friend's re-accept this session (breaks ping-pong)
} mesh_peer_t;

typedef enum {
    MESH_PROMPT_NONE = 0,
    MESH_PROMPT_OFFER,      // hold to friend
    MESH_PROMPT_WAITING,    // we held; waiting on the other rider
    MESH_PROMPT_DONE,       // both held: now friends
} mesh_prompt_state_t;

typedef enum {
    MESH_FRIEND_EVENT_ADDED = 1,   // a friendship completed on this terminal
    MESH_FRIEND_EVENT_UPDATED = 2, // a friend's look changed (their HELLO)
} mesh_friend_event_kind_t;

typedef struct {
    uint8_t kind;           // mesh_friend_event_kind_t
    mesh_friend_t record;
} mesh_friend_event_t;

typedef struct {
    bool enabled;
    bool has_identity;
    mesh_identity_t self;
    mesh_look_t look;       // our own; name comes from `self`
    uint32_t session;
    uint16_t seq;

    bool has_own_pos;
    mesh_pos_t own_pos;
    uint32_t own_pos_ms;

    mesh_peer_t peers[MESH_MAX_PEERS];
    mesh_friend_t friends[MESH_MAX_FRIENDS];
    size_t friend_count;
    uint32_t friends_generation; // bumped on every friend-list change, for the NVS saver

    uint8_t suppressed[MESH_MAX_SUPPRESSED][MESH_RIDER_ID_LEN];
    size_t suppressed_count;
    size_t suppressed_next;

    mesh_prompt_state_t prompt;
    uint8_t prompt_id[MESH_RIDER_ID_LEN];
    mesh_look_t prompt_look;
    uint32_t prompt_shown_ms;    // accumulated while the gate lets it show
    uint32_t prompt_done_at_ms;
    bool prompt_visible;

    uint32_t last_tick_ms;
    uint32_t last_hello_ms;
    uint32_t last_pos_ms;
    bool ticked;

    mesh_friend_event_t events[MESH_FRIEND_EVENTS];
    size_t event_head, event_count;
} mesh_core_t;

void mesh_core_init(mesh_core_t *c, uint32_t session_seed);
void mesh_core_set_enabled(mesh_core_t *c, bool enabled);
void mesh_core_set_identity(mesh_core_t *c, const mesh_identity_t *id); // NULL: none, mesh idles
void mesh_core_set_look(mesh_core_t *c, const mesh_look_t *look);
void mesh_core_set_own_pos(mesh_core_t *c, const mesh_pos_t *pos, uint32_t now_ms);

// Friend list (phone sync and NVS load). Upsert fails only when the list is full.
bool mesh_core_friend_upsert(mesh_core_t *c, const mesh_friend_t *f);
bool mesh_core_friend_remove(mesh_core_t *c, const uint8_t id[MESH_RIDER_ID_LEN]);
void mesh_core_friend_clear(mesh_core_t *c);
const mesh_friend_t *mesh_core_friend_find(const mesh_core_t *c, const uint8_t id[MESH_RIDER_ID_LEN]);

// Next friend event for the phone, oldest first. False when none are queued.
bool mesh_core_take_friend_event(mesh_core_t *c, mesh_friend_event_t *out);

void mesh_core_rx(mesh_core_t *c, const uint8_t *frame, size_t len, uint32_t now_ms, mesh_send_fn send,
                  void *ctx);
// `prompt_allowed` is mesh_core_prompt_gate() for the current nav view.
void mesh_core_tick(mesh_core_t *c, uint32_t now_ms, bool prompt_allowed, mesh_send_fn send, void *ctx);
// The rider held the ring on the prompt for `id`.
void mesh_core_accept(mesh_core_t *c, const uint8_t id[MESH_RIDER_ID_LEN], uint32_t now_ms, mesh_send_fn send,
                      void *ctx);

bool mesh_core_prompt_gate(bool navigating, uint32_t turn_distance_m, uint16_t speed_kmh_x10);

// The prompt to draw right now, if any (NONE while the gate holds it back).
mesh_prompt_state_t mesh_core_prompt(const mesh_core_t *c, uint8_t id[MESH_RIDER_ID_LEN], mesh_look_t *look);

typedef struct {
    const mesh_friend_t *f;
    float bearing_deg;      // true bearing from us, 0 = north
    float dist_m;
} mesh_friend_fix_t;

// Friends in range with a fresh position, when our own fix is fresh too. Returns the count.
size_t mesh_core_friends_in_range(const mesh_core_t *c, uint32_t now_ms, mesh_friend_fix_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
