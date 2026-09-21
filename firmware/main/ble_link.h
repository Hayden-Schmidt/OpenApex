#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// NimBLE peripheral: advertises the OpenApex nav service, accepts one bonded central (the
// Android relay), and decodes each incoming characteristic write into a raw_notif_t pushed onto
// raw_packet_queue. Board-specific BLE plumbing lives entirely in ble_link.c; the navigation
// model and packet decoder stay hardware-agnostic (see board_profile.h comment on this boundary).
void ble_link_init(QueueHandle_t raw_packet_queue);
