#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include <stdbool.h>
#include <stdint.h>

// Consecutive undecodable writes before the link is called faulted. Small enough that a rider
// notices within a second or two at the ~3 Hz publish rate, large enough that a couple of corrupt
// packets across a reconnect don't flash a warning.
#define BLE_LINK_FAULT_THRESHOLD 10u

// NimBLE peripheral: advertises the OpenApex nav service, accepts one bonded central (the
// Android relay), and decodes each incoming characteristic write into a raw_notif_t pushed onto
// raw_packet_queue, and each incoming-call write into a call_event_t on call_event_queue. Board-specific BLE plumbing lives entirely in ble_link.c; the navigation
// model and packet decoder stay hardware-agnostic (see board_profile.h comment on this boundary).
//
// Also serves the settings, device-info and avatar-upload characteristics (§5.6). NVS must already
// be initialised: main.c does it first, so saved settings are applied before the GUI starts.
void ble_link_init(QueueHandle_t raw_packet_queue, QueueHandle_t call_event_queue);

// True when a long run of writes has arrived that could not be decoded â€” a truncating or
// mismatched link. The display uses this to show a fault rather than holding the last good frame
// forever, which is what the terminal did for three whole sessions on 2026-09-24.
bool ble_link_is_faulted(void);

// True while the relay (BLE central) is connected. Drives the idle screen's phone-connected state.
bool ble_link_is_connected(void);

// Last negotiated ATT MTU, or BLE_ATT_MTU_DFLT (23) if the central never exchanged. Diagnostic.
uint16_t ble_link_negotiated_mtu(void);

// Tells the phone to answer call_id (CALL_ACTION notify, call_packet.h). False when the link is
// down or the phone has not subscribed; the ring then simply plays out on the phone.
bool ble_link_send_call_accept(uint16_t call_id);
