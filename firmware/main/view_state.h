#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "call_packet.h"
#include "mesh.h"
#include "nav_model.h"

// Render state the GUI consumes. The GUI reads this and only this — it never touches BLE, UART,
// or GNSS APIs directly. Produced by countdown_task, consumed by gui_task.
typedef enum {
    VIEW_IDLE = 0,   // no active navigation, no fabricated values
    VIEW_ACTIVE,     // maneuver + countdown distance + speed
    VIEW_STALE,      // last-known maneuver, de-emphasized (link/GNSS fresh-less)
    VIEW_ARRIVED,    // completion glyph
} view_state_t;

// Friends drawn on the compass at most; more in range than this is not a real ride.
#define VIEW_FRIENDS_MAX 8
// Riders on the Add friend card at most (one in the centre, the rest a swipe away).
#define VIEW_CANDIDATES_MAX 6

typedef struct {
    view_state_t state;
    nav_icon_t icon_type;
    uint32_t distance_meters;  // countdown-interpolated; meaningless when state != VIEW_ACTIVE
    bool stale;
    uint16_t speed_kmh_x10;    // 0xFFFF = unknown
    uint16_t heading_deg;      // 0xFFFF = unknown; fused by heading_fusion.c, not a raw passthrough
    float heading_confidence;  // 0..1; meaningless when heading_deg is unknown
    bool heading_frozen;       // heading held from the last good estimate rather than live
    uint8_t battery_percent;   // 0xFF = unknown
    char street_name[NAV_STREET_LEN];
    char eta[NAV_ETA_LEN];
    uint32_t sequence;

    // --- Idle-screen fields (design/screens/02-idle). Meaningful in every state, but
    // only VIEW_IDLE renders them today.
    bool phone_connected;          // BLE central connected; set by countdown_task from ble_link_is_connected()
    char clock[NAV_CLOCK_LEN];     // "HH:MM" local, from the phone's clock (packet v4); empty = never synced
    uint32_t odometer_meters;      // device odometer (odometer.c), NVS-persisted by odometer_store.c

    // --- Trip/traffic ring (design/screens/03-turn-by-turn, the "Google Trip Data" variant).
    // Source: the ProgressStyle segments of Google Maps' navigation notification (packet v4),
    // classified by normalize_traffic_color(); progress from android.progress/progressMax.
    nav_traffic_span_t traffic[NAV_TRAFFIC_MAX_SPANS];
    uint8_t traffic_count;            // 0 = no traffic data; the ring renders neutral, not "clear"
    uint16_t trip_progress_permille;  // 0..1000 of the whole trip; the ring is consumed up to here

    // --- Incoming call (call_state.c). Outranks every page while set; the nav fields above keep
    // updating underneath so the page comes back current when the call clears.
    bool call_ringing;
    bool call_active;              // answered; call_ringing is false while this is set
    uint32_t call_elapsed_s;       // since the call was answered (call_active only)
    uint16_t call_id;              // echo back in the accept (gui_app_set_call_accept_handler)
    char caller[CALL_CALLER_LEN];  // contact name, folded to ASCII; empty for an unknown caller
    char call_number[CALL_NUMBER_LEN];  // may be empty (withheld)

    // --- Rider mesh (mesh.h, docs/reference/rider_mesh_v1.md). The prompt ranks below a call and above
    // navigation; mesh_core already holds it back while the prompt gate is closed.
    mesh_prompt_state_t friend_prompt;             // MESH_PROMPT_NONE = no page
    uint8_t friend_prompt_id[MESH_RIDER_ID_LEN];  // echo back in the accept
    mesh_look_t friend_peer;                       // the other rider
    mesh_look_t friend_self;                       // us, as they see us
    mesh_friend_marker_t friends[VIEW_FRIENDS_MAX];  // in range with a fresh fix, true bearings
    uint8_t friend_count;
    // Add friend (blueprint 04 U1-U5): the pair card, and whether the menu may show at all (no call,
    // not in or near a turn). The GUI owns whether the menu is open (gui_app_set_friend_handlers).
    mesh_candidate_t friend_cands[VIEW_CANDIDATES_MAX];
    uint8_t friend_cand_count;
    bool friend_menu_allowed;
} terminal_view_state_t;
