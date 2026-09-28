#pragma once

#include "mesh_core.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ble_gatt_access_ctxt;

// Terminal glue for mesh_core (docs/OpenApex_SPEC.md §5.7): runs it over the board's radios
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

// The rider held the ring. Queued; the mesh task sends FRIEND_ACCEPT. Never blocks.
void mesh_request_accept(const uint8_t id[MESH_RIDER_ID_LEN]);

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
