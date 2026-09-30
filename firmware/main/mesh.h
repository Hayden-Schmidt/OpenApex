#pragma once

#include "mesh_core.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ble_gatt_access_ctxt;

// Terminal glue for mesh_core (docs/reference/rider_mesh_v1.md): runs it over the board's radios
// (mesh_transport.h), owns the one mesh_core_t behind a mutex, persists the rider identity and friend list in
// NVS and runs the mesh task. Every function here is safe from any task.

// Loads identity and friends. Requires NVS and settings to be loaded. Call before ble_link_init():
// the BLE characteristics use it.
void mesh_init(void);

// Starts the mesh task, which brings the transports up. Call after ble_link_init().
void mesh_start(void);

// Our own fix from the latest v5 packet. Unknown lat/lon clears nothing: the stale fix simply ages
// out of mesh_core after MESH_OWN_POS_MAX_AGE_MS.
void mesh_set_own_pos(const mesh_pos_t *pos);

// mesh_core_prompt_gate() for the current nav view; countdown_task refreshes it every tick.
void mesh_set_prompt_allowed(bool allowed);

// The rider held the ring on an incoming request (U4). Queued; the mesh task sends FRIEND_ACCEPT.
// Never blocks. `id` is the pseudonym from mesh_get_prompt().
void mesh_request_accept(const uint8_t id[MESH_RIDER_ID_LEN]);

// The friend menu is open (U1) or closed. While open, and for a grace after, our look is public so
// nearby riders can list us. Never blocks.
void mesh_set_menu_open(bool open);

typedef struct {
    uint8_t id[MESH_RIDER_ID_LEN]; // pseudonym: what mesh_request_friend() takes
    mesh_look_t look;
} mesh_nearby_t;

// Copies of the riders in range with a public look who are not friends yet (U2). Returns the count.
size_t mesh_get_nearby(mesh_nearby_t *out, size_t cap);

// The rider picked `id` from the list (U3). Queued; the mesh task sends FRIEND_REQUEST. Never blocks.
void mesh_request_friend(const uint8_t id[MESH_RIDER_ID_LEN]);

// Development console: gives a board that has never met the phone a MAC-derived identity (stored in
// NVS; the phone overwrites it on its first connect). False if one already exists.
bool mesh_debug_make_identity(void);

// Development console: sets our fix directly. While `lock_out_phone` is true the phone's fixes are
// ignored, so a simulated position is not interleaved with the real one. Pass false (and a NULL `pos`) to release.
void mesh_debug_set_pos(const mesh_pos_t *pos, bool lock_out_phone);

// Drops every friend on this terminal (development console; the phone list is untouched).
void mesh_forget_all_friends(void);

// The prompt to draw now. `peer` and `self` are filled when the result is not NONE.
mesh_prompt_state_t mesh_get_prompt(uint8_t id[MESH_RIDER_ID_LEN], mesh_look_t *peer, mesh_look_t *self);

typedef struct {
    mesh_look_t look;
    float bearing_deg; // true bearing, 0 = north
    float dist_m;
} mesh_friend_marker_t;

// Copies of the friends in range with a fresh position. Returns the count.
size_t mesh_get_friends_in_range(mesh_friend_marker_t *out, size_t cap);

// Access callback bodies for the identity (0x0008) and friends (0x0009) characteristics.
int mesh_identity_ble_access(struct ble_gatt_access_ctxt *ctxt);
int mesh_friends_ble_access(struct ble_gatt_access_ctxt *ctxt);

#ifdef __cplusplus
}
#endif
