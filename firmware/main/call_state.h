#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "call_packet.h"
#include "view_state.h"

// Incoming-call state, owned by countdown_task alongside the nav pipeline. Host-testable: no
// FreeRTOS or BLE, time injected.
//
// The phone re-sends RING every CALL_RING_REFRESH_MS while the call still rings. A ring not
// refreshed within CALL_RING_TIMEOUT_MS -- or any link drop -- clears the call, so a lost CLEAR can
// never leave a stale "incoming call" over the nav page.
#define CALL_RING_REFRESH_MS 5000u
#define CALL_RING_TIMEOUT_MS 12000u

void call_state_reset(void);

void call_state_apply(const call_event_t *event, uint32_t now_ms);

// Expires a ring that stopped being refreshed, or clears it when the phone link is gone.
void call_state_tick(uint32_t now_ms, bool link_connected);

// Copies the call fields into a view state; the nav fields are untouched.
void call_state_fill(terminal_view_state_t *out);
