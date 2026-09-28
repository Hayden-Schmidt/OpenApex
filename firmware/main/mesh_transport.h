#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// One radio the rider mesh can talk over (docs/OpenApex_SPEC.md §5.7). mesh_core never sees these:
// mesh.c sends every frame on every transport the board has and funnels what they receive, with
// duplicates dropped, into the one queue the mesh task drains.
//
// BLE extended advertising on the Coded PHY is the baseline every terminal speaks, so any two
// terminals can meet. ESP-NOW is an extra on boards with the RAM for Wi-Fi (not the C3), giving
// S3-to-S3 more range and bandwidth; those boards run both.

// Called from the transport's own task context: copy the frame out and return.
typedef void (*mesh_rx_fn)(const uint8_t *frame, size_t len);

typedef struct {
    const char *name;
    // Brings the radio up. False when it cannot; the mesh runs on whatever else started.
    bool (*start)(mesh_rx_fn rx);
    void (*send)(const uint8_t *frame, size_t len);
    // Called every mesh-task pass. NULL when the transport has no timed work.
    void (*poll)(uint32_t now_ms);
} mesh_transport_t;

extern const mesh_transport_t mesh_transport_ble;
extern const mesh_transport_t mesh_transport_espnow;
