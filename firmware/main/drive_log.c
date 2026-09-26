#include "drive_log.h"

#if defined(OPENAPEX_DRIVE_LOG) && OPENAPEX_DRIVE_LOG

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

// The strncpy()s below deliberately clip strings into fixed-width, pre-zeroed log fields -- the
// truncation -O2 warns about is the point, and the zeroed tail keeps every field terminated.
#pragma GCC diagnostic ignored "-Wstringop-truncation"

static const char *TAG = "drive_log";

#define LOG_QUEUE_LEN 8
#define SECTOR_SIZE 4096U
#define RECORDS_PER_SECTOR (SECTOR_SIZE / DRIVE_LOG_RECORD_SIZE) // 32

static const esp_partition_t *s_partition;
static QueueHandle_t s_queue;
static uint32_t s_total_slots;
static uint32_t s_next_slot;    // ring slot the next record goes into
static uint32_t s_next_index;   // monotonic record_index for the next record
static uint32_t s_boot_id;
static bool s_enabled;

// Last-logged tier 2/3 payloads, for the change-only filter (see header). Compared byte-wise with
// the sequence field zeroed, since the sequence changes on every re-post of identical content.
static drive_log_model_t s_last_model;
static drive_log_view_t s_last_view;
static bool s_has_last_model;
static bool s_has_last_view;

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Recovers the ring cursor by finding the highest record_index still on flash; the next write goes
// into the following slot. Survives power loss with no NVS bookkeeping: the flash content is the
// only state, so a cut cable mid-drive costs at most the one torn record.
static void recover_cursor(void) {
    uint32_t best_index = 0;
    uint32_t best_slot = 0;
    bool found = false;

    for (uint32_t slot = 0; slot < s_total_slots; slot++) {
        drive_log_record_t rec;
        // Only the 16-byte header is needed to order the ring.
        if (esp_partition_read(s_partition, slot * DRIVE_LOG_RECORD_SIZE, &rec, 16) != ESP_OK) {
            continue;
        }
        if (rec.magic != DRIVE_LOG_MAGIC || rec.version != DRIVE_LOG_VERSION) {
            continue;
        }
        if (!found || rec.record_index >= best_index) {
            best_index = rec.record_index;
            best_slot = slot;
            found = true;
        }
    }

    if (found) {
        s_next_slot = (best_slot + 1) % s_total_slots;
        s_next_index = best_index + 1;
    } else {
        s_next_slot = 0;
        s_next_index = 0;
    }
}

// Appends one record, erasing the sector ahead when the cursor crosses into it. Erase-on-entry
// (rather than erase-on-wrap) keeps every slot between the cursor and the end of the current
// sector known-free, so no read-modify-write is ever needed.
static void write_record(drive_log_record_t *rec) {
    rec->magic = DRIVE_LOG_MAGIC;
    rec->version = DRIVE_LOG_VERSION;
    rec->record_index = s_next_index;
    rec->uptime_ms = now_ms();
    rec->boot_id = s_boot_id;

    const uint32_t offset = s_next_slot * DRIVE_LOG_RECORD_SIZE;
    if (s_next_slot % RECORDS_PER_SECTOR == 0) {
        if (esp_partition_erase_range(s_partition, offset, SECTOR_SIZE) != ESP_OK) {
            ESP_LOGW(TAG, "sector erase failed at 0x%08lx, disabling", (unsigned long)offset);
            s_enabled = false;
            return;
        }
    }
    if (esp_partition_write(s_partition, offset, rec, DRIVE_LOG_RECORD_SIZE) != ESP_OK) {
        ESP_LOGW(TAG, "record write failed at 0x%08lx, disabling", (unsigned long)offset);
        s_enabled = false;
        return;
    }

    s_next_index++;
    s_next_slot = (s_next_slot + 1) % s_total_slots;
}

// Dedicated low-priority task so a flash erase (which can stall for milliseconds) never happens on
// the NimBLE callback or the GUI tick.
static void drive_log_task(void *argument) {
    (void)argument;
    drive_log_record_t rec;
    for (;;) {
        if (xQueueReceive(s_queue, &rec, portMAX_DELAY) == pdTRUE && s_enabled) {
            write_record(&rec);
        }
    }
}

// Never blocks: dropping a log record is always preferable to stalling navigation.
static void enqueue(uint8_t kind, const void *payload, size_t len) {
    if (!s_enabled || s_queue == NULL) {
        return;
    }
    drive_log_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.kind = kind;
    if (payload != NULL && len > 0) {
        memcpy(rec.payload, payload, len > sizeof(rec.payload) ? sizeof(rec.payload) : len);
    }
    (void)xQueueSend(s_queue, &rec, 0);
}

void drive_log_init(void) {
    s_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                           (esp_partition_subtype_t)DRIVE_LOG_PARTITION_SUBTYPE, NULL);
    if (s_partition == NULL) {
        // Flashed against a production partition table. Log and carry on unlogged rather than
        // refusing to boot -- the terminal's job is navigation, not diagnostics.
        ESP_LOGW(TAG, "no log partition found; drive logging disabled");
        return;
    }
    s_total_slots = s_partition->size / DRIVE_LOG_RECORD_SIZE;
    if (s_total_slots == 0) {
        ESP_LOGW(TAG, "log partition too small; drive logging disabled");
        return;
    }
    s_boot_id = esp_random();
    recover_cursor();
    s_queue = xQueueCreate(LOG_QUEUE_LEN, sizeof(drive_log_record_t));
    if (s_queue == NULL) {
        ESP_LOGW(TAG, "queue alloc failed; drive logging disabled");
        return;
    }
    s_enabled = true;
    xTaskCreate(drive_log_task, "drive_log", 3072, NULL, 2, NULL);
    ESP_LOGI(TAG, "drive log ready: %lu slots, resuming at slot %lu index %lu, boot_id=0x%08lx",
             (unsigned long)s_total_slots, (unsigned long)s_next_slot,
             (unsigned long)s_next_index, (unsigned long)s_boot_id);
}

void drive_log_boot(const char *what) {
    char payload[112];
    memset(payload, 0, sizeof(payload));
    snprintf(payload, sizeof(payload), "%s reset=%d", what == NULL ? "" : what, (int)esp_reset_reason());
    enqueue(DRIVE_LOG_KIND_BOOT, payload, sizeof(payload));
}

void drive_log_ble(drive_log_ble_event_t event, int32_t detail) {
    struct __attribute__((packed)) {
        uint8_t event;
        int32_t detail;
    } payload = {(uint8_t)event, detail};
    enqueue(DRIVE_LOG_KIND_BLE, &payload, sizeof(payload));
}

void drive_log_raw(const raw_notif_t *raw) {
    if (raw == NULL) return;
    drive_log_raw_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.sequence = raw->sequence;
    payload.icon_rotation_deg = raw->icon_rotation_deg;
    payload.speed_kmh_x10 = raw->speed_kmh_x10;
    payload.heading_deg = raw->heading_deg;
    payload.battery_percent = raw->battery_percent;
    payload.gnss_fix_valid = raw->gnss_fix_valid ? 1 : 0;
    payload.progress = (uint32_t)raw->progress;
    payload.progress_max = (uint32_t)raw->progress_max;
    strncpy(payload.title, raw->title_str, sizeof(payload.title) - 1);
    strncpy(payload.distance, raw->distance_str, sizeof(payload.distance) - 1);
    strncpy(payload.eta, raw->eta_str, sizeof(payload.eta) - 1);
    enqueue(DRIVE_LOG_KIND_RAW, &payload, sizeof(payload));
}

void drive_log_model(const nav_model_t *model) {
    if (model == NULL) return;
    drive_log_model_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.sequence = model->sequence;
    payload.icon_type = (uint8_t)model->icon_type;
    payload.distance_meters = model->distance_meters;
    payload.remaining_meters = model->remaining_meters;
    payload.speed_kmh_x10 = model->speed_kmh_x10;
    payload.heading_deg = model->heading_deg;
    strncpy(payload.street, model->street_name, sizeof(payload.street) - 1);

    // Change-only: compare with the sequence masked out, since the sequence increments on every
    // re-post of byte-identical content.
    drive_log_model_t probe = payload;
    probe.sequence = 0;
    drive_log_model_t last = s_last_model;
    last.sequence = 0;
    if (s_has_last_model && memcmp(&probe, &last, sizeof(probe)) == 0) {
        return;
    }
    s_last_model = payload;
    s_has_last_model = true;
    enqueue(DRIVE_LOG_KIND_MODEL, &payload, sizeof(payload));
}

void drive_log_view(const terminal_view_state_t *view) {
    if (view == NULL) return;
    drive_log_view_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.sequence = view->sequence;
    payload.state = (uint8_t)view->state;
    payload.icon_type = (uint8_t)view->icon_type;
    payload.stale = view->stale ? 1 : 0;
    payload.distance_meters = view->distance_meters;
    payload.speed_kmh_x10 = view->speed_kmh_x10;
    payload.heading_deg = view->heading_deg;
    payload.battery_percent = view->battery_percent;

    // As above, plus distance: the countdown interpolates it every 100ms tick, so logging on any
    // change would write ~10 records/second. Only whole-10-metre steps are interesting.
    drive_log_view_t probe = payload;
    probe.sequence = 0;
    probe.distance_meters = payload.distance_meters / 10;
    drive_log_view_t last = s_last_view;
    last.sequence = 0;
    last.distance_meters = s_last_view.distance_meters / 10;
    if (s_has_last_view && memcmp(&probe, &last, sizeof(probe)) == 0) {
        return;
    }
    s_last_view = payload;
    s_has_last_view = true;
    enqueue(DRIVE_LOG_KIND_VIEW, &payload, sizeof(payload));
}

#endif // OPENAPEX_DRIVE_LOG
