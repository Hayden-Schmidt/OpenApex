#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "nav_model.h"
#include "packet.h"
#include "view_state.h"

// Drive logger — DEVELOPMENT ONLY.
// ---------------------------------
// Records what the terminal received, parsed and displayed to a dedicated flash partition, so a
// drive can be taken with no laptop attached and the whole session pulled afterwards over USB:
//
//   esptool.py --port COM30 read_flash 0x190000 0x100000 esp_log.bin
//   python tools/decode_drive.py esp_log.bin
//
// The three record tiers (RAW / MODEL / VIEW) exist because the interesting bugs live *between*
// them: bytes can arrive intact and still be misparsed, and a correct parse can still be mangled
// by the countdown/state machine afterwards. Logging only one tier identifies that something went
// wrong without narrowing down where.
//
// GATED OFF BY DEFAULT. Every function below compiles to a no-op unless OPENAPEX_DRIVE_LOG=1 is
// defined, which only the `prototype_c3_devlog` PlatformIO env does. The shipping `prototype_c3`
// env must never enable it: it burns flash write cycles, costs a partition, and records the
// rider's route.

#define DRIVE_LOG_RECORD_SIZE 128U
#define DRIVE_LOG_MAGIC 0xA7C3U
#define DRIVE_LOG_VERSION 1U

// Custom data-partition subtype claimed for the log ring (see partitions.csv).
#define DRIVE_LOG_PARTITION_SUBTYPE 0x9A

typedef enum {
    DRIVE_LOG_KIND_BOOT = 1,  // session marker: firmware build + reset reason
    DRIVE_LOG_KIND_BLE = 2,   // link lifecycle (connect/disconnect/decode failure)
    DRIVE_LOG_KIND_RAW = 3,   // tier 1: raw_notif_t as decoded off the wire
    DRIVE_LOG_KIND_MODEL = 4, // tier 2: nav_model_t as produced by normalize_packet()
    DRIVE_LOG_KIND_VIEW = 5,  // tier 3: terminal_view_state_t as handed to the GUI
} drive_log_kind_t;

typedef enum {
    DRIVE_LOG_BLE_CONNECTED = 1,
    DRIVE_LOG_BLE_DISCONNECTED = 2,
    DRIVE_LOG_BLE_DECODE_FAILED = 3,
    DRIVE_LOG_BLE_QUEUE_FULL = 4,
} drive_log_ble_event_t;

// 16-byte common header + 112-byte payload. Fixed size keeps the flash ring trivially indexable
// (32 records per 4096-byte sector) and makes a torn write cost exactly one record.
typedef struct __attribute__((packed)) {
    uint16_t magic;        // DRIVE_LOG_MAGIC; 0xFFFF means "erased/free slot"
    uint8_t version;       // DRIVE_LOG_VERSION
    uint8_t kind;          // drive_log_kind_t
    uint32_t record_index; // monotonic across boots — the ring's ordering key
    uint32_t uptime_ms;    // ms since this boot
    uint32_t boot_id;      // random per boot; groups records into a session
    uint8_t payload[DRIVE_LOG_RECORD_SIZE - 16];
} drive_log_record_t;

_Static_assert(sizeof(drive_log_record_t) == DRIVE_LOG_RECORD_SIZE, "drive log record must be 128B");

// Tier 1. Mirrors the wire fields; the phone's own log holds the notification this came from, and
// `sequence` is the join key between the two sides.
typedef struct __attribute__((packed)) {
    uint32_t sequence;
    int16_t icon_rotation_deg;
    uint16_t speed_kmh_x10;
    uint16_t heading_deg;
    uint8_t battery_percent;
    uint8_t gnss_fix_valid;
    int32_t progress;     // -1 = unknown, as on the wire
    int32_t progress_max; // -1 = unknown
    char title[RAW_TITLE_STR_LEN];  // 64
    char distance[RAW_DIST_STR_LEN]; // 16
    char eta[12];                    // truncated: full ETA is in the phone-side log
} drive_log_raw_t;

// Tier 2.
typedef struct __attribute__((packed)) {
    uint32_t sequence;
    uint8_t icon_type;
    int32_t distance_meters;
    int32_t remaining_meters;
    uint16_t speed_kmh_x10;
    uint16_t heading_deg;
    char street[NAV_STREET_LEN]; // 64
} drive_log_model_t;

// Tier 3.
typedef struct __attribute__((packed)) {
    uint32_t sequence;
    uint8_t state;
    uint8_t icon_type;
    uint8_t stale;
    uint32_t distance_meters;
    uint16_t speed_kmh_x10;
    uint16_t heading_deg;
    uint8_t battery_percent;
} drive_log_view_t;

// The payload must fit the fixed record; tools/decode_drive.py unpacks these exact layouts.
_Static_assert(sizeof(drive_log_raw_t) <= DRIVE_LOG_RECORD_SIZE - 16, "raw payload too large");
_Static_assert(sizeof(drive_log_model_t) <= DRIVE_LOG_RECORD_SIZE - 16, "model payload too large");
_Static_assert(sizeof(drive_log_view_t) <= DRIVE_LOG_RECORD_SIZE - 16, "view payload too large");

#if defined(OPENAPEX_DRIVE_LOG) && OPENAPEX_DRIVE_LOG

// Locates the log partition, recovers the ring write cursor, and starts the writer task. Safe to
// call when the partition is missing — logging then disables itself rather than failing the boot.
void drive_log_init(void);

void drive_log_boot(const char *what);
void drive_log_ble(drive_log_ble_event_t event, int32_t detail);
void drive_log_raw(const raw_notif_t *raw);
// MODEL/VIEW are logged only when the derived values actually change: Maps re-posts several times
// a second and the view ticks at 10Hz, and logging every one would fill the ring with duplicates
// and burn flash for nothing.
void drive_log_model(const nav_model_t *model);
void drive_log_view(const terminal_view_state_t *view);

#else

// Production build: every call vanishes. Arguments are still type-checked.
static inline void drive_log_init(void) {}
static inline void drive_log_boot(const char *what) { (void)what; }
static inline void drive_log_ble(drive_log_ble_event_t event, int32_t detail) { (void)event; (void)detail; }
static inline void drive_log_raw(const raw_notif_t *raw) { (void)raw; }
static inline void drive_log_model(const nav_model_t *model) { (void)model; }
static inline void drive_log_view(const terminal_view_state_t *view) { (void)view; }

#endif
