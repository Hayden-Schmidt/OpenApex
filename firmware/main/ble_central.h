#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// NimBLE central: scans for the Android relay's GATT service, connects, subscribes to the
// navigation characteristic, and pushes each decoded raw_notif_t into raw_packet_queue.
// Board-specific BLE plumbing lives entirely in ble_central.c; the navigation model and packet
// decoder stay hardware-agnostic (see board_profile.h comment on this boundary).
void ble_central_init(QueueHandle_t raw_packet_queue);
