#include "countdown.h"
#include "packet.h"
#include "pipeline.h"
#include "view_state.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "openapex";

#define RAW_PACKET_QUEUE_LEN 8

static QueueHandle_t raw_packet_queue;
static SemaphoreHandle_t view_mutex;
static terminal_view_state_t shared_view;

// BLE service/characteristic the phone advertises (matches the Android relay).
// Open decision: BLE central stack init is pinned once the ESP-IDF version is chosen.
// static const char *SERVICE_UUID = "c9c6d0a0-0001-4f0a-9c8e-2f6b1a2d3e4f";
// static const char *CHAR_UUID = "c9c6d0a0-0002-4f0a-9c8e-2f6b1a2d3e4f";

uint32_t platform_now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void ble_handler_task(void *argument) {
    (void)argument;
    // Phase 1 TODO (Slice C, blocked on ESP-IDF version): initialize NimBLE central, scan and
    // connect to the phone GATT server, subscribe to the nav characteristic, and push decoded
    // raw_notif_t packets into raw_packet_queue. The decode/normalize/countdown pipeline below
    // is complete and host-tested; only the GATT stack integration remains.
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
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
            xSemaphoreGive(view_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void gui_task(void *argument) {
    (void)argument;
    // Phase 1 TODO (Slice D): initialize the selected GC9A01 profile + LVGL, and render
    // shared_view each frame. C++ screen classes live in this task's render path.
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "OpenApex terminal boot: PROTOTYPE_C3_GC9A01");
    countdown_reset();
    memset(&shared_view, 0, sizeof(shared_view));
    shared_view.state = VIEW_IDLE;

    raw_packet_queue = xQueueCreate(RAW_PACKET_QUEUE_LEN, sizeof(raw_notif_t));
    view_mutex = xSemaphoreCreateMutex();

    xTaskCreate(ble_handler_task, "ble_handler_task", 4096, NULL, 4, NULL);
    xTaskCreate(countdown_task, "countdown_task", 4096, NULL, 4, NULL);
    xTaskCreate(gui_task, "gui_task", 4096, NULL, 5, NULL);
}
