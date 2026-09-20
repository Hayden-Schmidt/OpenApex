#include "countdown.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "openapex";

static void ble_handler_task(void *argument) {
    (void)argument;
    for (;;) {
        // Phase 1 TODO: BLE central, notification subscription, and packet decoder.
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void countdown_task(void *argument) {
    (void)argument;
    for (;;) {
        // Phase 1 TODO: feed decoded packets and publish countdown_output_t to the GUI.
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void gui_task(void *argument) {
    (void)argument;
    for (;;) {
        // Phase 1 TODO: initialize the selected GC9A01 profile and LVGL dial.
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "OpenApex terminal boot: PROTOTYPE_C3_GC9A01");
    countdown_reset();
    xTaskCreate(ble_handler_task, "ble_handler_task", 4096, NULL, 4, NULL);
    xTaskCreate(countdown_task, "countdown_task", 2048, NULL, 4, NULL);
    xTaskCreate(gui_task, "gui_task", 4096, NULL, 5, NULL);
}
