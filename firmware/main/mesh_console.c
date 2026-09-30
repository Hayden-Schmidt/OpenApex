#include "mesh_console.h"

#if defined(OPENAPEX_MESH_CONSOLE) && OPENAPEX_MESH_CONSOLE

#include <stdio.h>
#include <string.h>

#include "hal/usb_serial_jtag_ll.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "meshcon";

#define LINE_MAX_LEN 64
#define HEAP_LOG_PERIOD_MS 10000

static void log_heap(void) {
    ESP_LOGI(TAG, "heap free=%u min=%u largest=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static void run_command(const char *line) {
    if (strcmp(line, "heap") == 0) {
        log_heap();
    } else if (line[0] != '\0') {
        ESP_LOGW(TAG, "unknown command '%s'", line);
    }
}

static void console_task(void *arg) {
    (void)arg;
    log_heap(); // the baseline, before the console's driver takes its own share
    char line[LINE_MAX_LEN];
    size_t n = 0;
    TickType_t heap_at = xTaskGetTickCount();
    for (;;) {
        // Polled straight from the peripheral: the driver's IRAM ISR cost ~3.3 KB of heap, which
        // is more than the boot margin (see log_heap at start).
        uint8_t ch;
        if (usb_serial_jtag_ll_rxfifo_data_available() && usb_serial_jtag_ll_read_rxfifo(&ch, 1) == 1) {
            if (ch == '\n' || ch == '\r') {
                line[n] = '\0';
                run_command(line);
                n = 0;
            } else if (n < sizeof(line) - 1) {
                line[n++] = (char)ch;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (xTaskGetTickCount() - heap_at >= pdMS_TO_TICKS(HEAP_LOG_PERIOD_MS)) {
            heap_at = xTaskGetTickCount();
            log_heap();
        }
    }
}

// Boot heap is nearly exhausted: creating the task here (its stack is allocated up front) made the
// display's DMA buffers fail with NO_MEM. So the task is created after gui_task has allocated them.
static void spawn_console(void *arg) {
    (void)arg;
    xTaskCreate(console_task, "mesh_console", 3072, NULL, 2, NULL);
}

void mesh_console_start(void) {
    static const esp_timer_create_args_t args = {.callback = spawn_console, .name = "meshcon"};
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) == ESP_OK) esp_timer_start_once(t, 5000000);
}

#endif
