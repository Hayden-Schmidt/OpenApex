#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ancs_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// The terminal's GATT-CLIENT role, on the same connection where it is already the peripheral and
// GATT server (docs/OpenApex_SPEC.md §5.2, §5.5).
//
// Central/peripheral and client/server are orthogonal: the phone connects to us, and over that
// one link we serve our own service to the phone AND read the phone's Apple services. That is
// mandatory rather than clever -- ANCS is only available to an accessory acting as GATT client,
// and on iOS there is no other way to see a notification at all.
//
// Two services are consumed:
//   ANCS  - notifications, which on iOS is the whole nav source (Google Maps posts turn-by-turn
//           under CategoryIDLocation) and the whole incoming-call source.
//   CTS   - the wall clock, because the C3 has no RTC and on iOS there is no packet to carry one
//           whenever the companion app is asleep, which iOS will make it be.
// Both work with no app running, which is the property the whole iOS design rests on.
//
// ANCS is not guaranteed to be present: iOS publishes and unpublishes it, so this subscribes to
// GATT Service Changed and rediscovers rather than assuming one discovery pass is final.

typedef struct {
    // A notification's attributes have arrived, or it was removed. Called from the NimBLE host
    // task: do not block, queue instead.
    void (*on_notification)(const ancs_notification_t *n, bool removed);
    // Phone wall clock, seconds, LOCAL time (see cts_client.h). 0 is never reported.
    void (*on_time)(uint32_t local_epoch_s);
    // Raw byte tap for the capture tier. May be NULL.
    ancs_raw_fn on_raw;
} apple_client_cfg_t;

void apple_client_init(const apple_client_cfg_t *cfg);

// Link lifecycle, called from ble_link.c's GAP callback.
void apple_client_on_connect(uint16_t conn_handle);
void apple_client_on_disconnect(void);
// Encryption came up. Discovery starts here and not on connect: every ANCS characteristic
// requires an encrypted link, so discovering before the bond exists just earns an ATT error.
void apple_client_on_encryption(uint16_t conn_handle, int status);

// Routes an incoming notification/indication. Returns true if it was ours.
struct ble_gap_event;
bool apple_client_on_notify_rx(struct ble_gap_event *event);

// Retries a discovery that failed or found nothing. Called from countdown_task; cheap when idle.
void apple_client_tick(uint32_t now_ms);

// Answers (POSITIVE) or declines (NEGATIVE) a notification. The call Accept hold goes here on
// iOS: there is no CALL_ACTION characteristic in that direction, the phone is the server.
bool apple_client_perform_action(uint32_t uid, ancs_action_id_t action);

// True once ANCS is discovered and both its notify characteristics are subscribed.
bool apple_client_ancs_ready(void);

// Logs the discovered handle table and the live notification table. The day-one answer to "what
// did iOS actually do", without a rebuild.
void apple_client_dump(void);

#ifdef __cplusplus
}
#endif
