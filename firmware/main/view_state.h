#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "nav_model.h"

// Render state the GUI consumes. The GUI reads this and only this — it never touches BLE, UART,
// or GNSS APIs directly. Produced by countdown_task, consumed by gui_task.
typedef enum {
    VIEW_IDLE = 0,   // no active navigation, no fabricated values
    VIEW_ACTIVE,     // maneuver + countdown distance + speed
    VIEW_STALE,      // last-known maneuver, de-emphasized (link/GNSS fresh-less)
    VIEW_ARRIVED,    // completion glyph
} view_state_t;

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

    // --- Idle-screen fields (design reference/2. Idle Screen). Meaningful in every state, but
    // only VIEW_IDLE renders them today. All three are BACKEND GAPS: nothing populates them yet,
    // so they stay at their zero/unknown values outside firmware/sim_lvgl's fixture.
    bool phone_connected;          // GAP: ble_link.c logs connect/disconnect but does not publish it
    char clock[NAV_CLOCK_LEN];     // "HH:MM"; empty = unknown. GAP: no RTC on the C3, no phone time sync
    uint32_t odometer_meters;      // GAP: no odometer data model or NVS persistence yet
} terminal_view_state_t;
