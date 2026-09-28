#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ancs_core.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

// Wires the Apple services client into the queues the pipeline already consumes, so the iOS path
// joins the existing chain rather than growing a second one:
//
//   ANCS notification (nav)  -> ancs_nav_to_raw -> raw_packet_queue -> normalize -> countdown
//   ANCS notification (call) -> call_event_t    -> call_event_queue -> call_state -> CallScreen
//   CTS current time         -> pipeline_set_wall_clock
//
// Everything downstream of those queues is untouched and source-agnostic, which is the property
// SPEC 2.4 and 5.5 were written to preserve.
//
// Call it after apple_client_init() and before ble_link_init().
void ancs_bridge_init(QueueHandle_t raw_packet_queue, QueueHandle_t call_event_queue);

// The configured apple_client callbacks. Passed to apple_client_init().
void ancs_bridge_on_notification(const ancs_notification_t *n, bool removed);
void ancs_bridge_on_time(uint32_t local_epoch_s);

// Periodic work, from countdown_task. Re-sends the ring that call_state expects every 5 s: on
// Android the phone re-posts it, but ANCS announces a notification once and then says nothing
// until it changes, so without this a ring would time out at 12 s while the phone is still
// ringing (call_state.h).
void ancs_bridge_tick(uint32_t now_ms);

// Answers the call currently on screen, if it came from ANCS. Returns false when it did not, so
// the caller falls through to the Android CALL_ACTION notify path.
bool ancs_bridge_accept_call(uint16_t call_id);

#ifdef __cplusplus
}
#endif
