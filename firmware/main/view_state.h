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
    uint16_t heading_deg;      // 0xFFFF = unknown
    uint8_t battery_percent;   // 0xFF = unknown
    char street_name[NAV_STREET_LEN];
    char eta[NAV_ETA_LEN];
    uint32_t sequence;
} terminal_view_state_t;
