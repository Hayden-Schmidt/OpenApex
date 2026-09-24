#include "ble_link.h"
#include "countdown.h"
#include "drive_log.h"
#include "display_driver.h"
#include "gui_app.hpp"
#include "packet.h"
#include "pipeline.h"
#include "view_state.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <string.h>

static const char *TAG = "openapex";

#define RAW_PACKET_QUEUE_LEN 8

// Concurrency model
// ------------------
// raw_packet_queue: single-producer (ble_handler_task) / single-consumer (countdown_task) bounded
//   FreeRTOS queue of decoded raw_notif_t. ble_handler_task must never block on a full queue for
//   longer than a short bounded wait — BLE callbacks must not stall on rendering or countdown work.
// view_mutex + shared_view: single-writer (countdown_task only) / multi-reader (gui_task, and
//   later diagnostics/system_manager_task) shared terminal_view_state_t. countdown_task is the
//   only task allowed to write shared_view, always under view_mutex. Any other task — gui_task
//   included — MUST read it only through view_state_snapshot() below, never by touching
//   shared_view directly. countdown.c's own internal state (current/filtered_speed_kmh/
//   has_baseline) is private to countdown_task's call path and must not be called from any other
//   task.
static QueueHandle_t raw_packet_queue;
static SemaphoreHandle_t view_mutex;
static terminal_view_state_t shared_view;

// Copies the latest view state under view_mutex. This is the only sanctioned read path for
// shared_view outside of countdown_task; gui_task and any future reader task must call this
// instead of touching shared_view directly.
static void view_state_snapshot(terminal_view_state_t *out) {
    if (xSemaphoreTake(view_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        *out = shared_view;
        xSemaphoreGive(view_mutex);
    }
}

uint32_t platform_now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void countdown_task(void *argument) {
    (void)argument;
    raw_notif_t raw;
    for (;;) {
        while (xQueueReceive(raw_packet_queue, &raw, 0) == pdTRUE) {
            terminal_view_state_t next;
            memset(&next, 0, sizeof(next));
            view_state_apply_packet(&raw, platform_now_ms(), &next);
            if (xSemaphoreTake(view_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                shared_view = next;
                xSemaphoreGive(view_mutex);
            }
        }

        uint32_t now = platform_now_ms();
        if (xSemaphoreTake(view_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            view_state_tick(now, &shared_view);
            // A link that is delivering writes we cannot decode is NOT the same as no link and
            // NOT the same as "not navigating", but the terminal used to render all three
            // identically. On 2026-09-24 three whole sessions sat on the idle screen while the
            // phone believed it was relaying: every 146-byte packet was truncated to 20 bytes by
            // an unnegotiated ATT MTU and discarded here, so the view never advanced past boot.
            // The rider had no way to tell a quiet road from a broken link.
            //
            // Marking the view stale greys the dial (dial_screen.cpp), which at least says
            // "what you are looking at is not live". A dedicated fault screen naming the cause
            // would be better and is follow-up work -- gui_app only distinguishes idle from dial
            // today, so a new state means a new screen class.
            if (ble_link_is_faulted()) {
                shared_view.stale = true;
                if (shared_view.state == VIEW_ACTIVE) {
                    shared_view.state = VIEW_STALE;
                }
            }
            xSemaphoreGive(view_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void gui_task(void *argument) {
    (void)argument;
    // C++ screen classes (firmware/gui, §16.6) live behind gui_app_init/gui_app_update and must
    // only ever see the local snapshot below, never shared_view or view_mutex directly.
    lv_init();
    lv_tick_set_cb(platform_now_ms);
    display_driver_init();
    gui_app_init();
    for (;;) {
        terminal_view_state_t frame;
        view_state_snapshot(&frame);
        gui_app_update(&frame);
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "OpenApex terminal boot: PROTOTYPE_C3_GC9A01");
    pipeline_reset();
    memset(&shared_view, 0, sizeof(shared_view));
    shared_view.state = VIEW_IDLE;

    raw_packet_queue = xQueueCreate(RAW_PACKET_QUEUE_LEN, sizeof(raw_notif_t));
    view_mutex = xSemaphoreCreateMutex();

    // Dev-only; no-ops entirely in the production build. Started before ble_link_init so the very
    // first connect/packet of a session is captured.
    drive_log_init();
    drive_log_boot("PROTOTYPE_C3_GC9A01");

    // ble_link_init spawns NimBLE's own host task (single-producer into raw_packet_queue); there
    // is no separate ble_handler_task to create.
    ble_link_init(raw_packet_queue);
    xTaskCreate(countdown_task, "countdown_task", 4096, NULL, 4, NULL);
    // 4096 was enough while the distance label only ever rendered "" (distance was always unknown
    // pre-fix, see pipeline.c), so LVGL's font/glyph rendering path was never exercised on real
    // hardware and its stack use went unnoticed until the first live packet with a known distance
    // caused a stack protection fault here.
    xTaskCreate(gui_task, "gui_task", 8192, NULL, 5, NULL);
}
