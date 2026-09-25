#include "odometer_store.h"

#include "odometer.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "odometer_store";

#define ODOMETER_STORE_NAMESPACE "odometer"
#define ODOMETER_STORE_KEY "meters"
#define ODOMETER_STORE_SAVE_INTERVAL_MS (30U * 1000U)
#define ODOMETER_STORE_MIN_STEP_M 100U

static uint32_t s_saved_meters;
static uint32_t s_last_save_ms;
static bool s_have_last_save;

void odometer_store_load(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ODOMETER_STORE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        // Expected on first-ever boot / a freshly erased partition.
        ESP_LOGI(TAG, "odometer_store_load: no saved total (%s)", esp_err_to_name(err));
        return;
    }
    uint32_t meters = 0;
    err = nvs_get_u32(handle, ODOMETER_STORE_KEY, &meters);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "odometer_store_load: no saved total (%s)", esp_err_to_name(err));
        return;
    }
    odometer_restore(meters);
    s_saved_meters = meters;
    ESP_LOGI(TAG, "odometer_store_load: restored %lu m", (unsigned long)meters);
}

void odometer_store_maybe_save(uint32_t now_ms) {
    uint32_t meters = odometer_meters();
    if (meters < s_saved_meters + ODOMETER_STORE_MIN_STEP_M) {
        return;
    }
    if (s_have_last_save && (now_ms - s_last_save_ms) < ODOMETER_STORE_SAVE_INTERVAL_MS) {
        return;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(ODOMETER_STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "odometer_store_maybe_save: nvs_open failed (%s)", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u32(handle, ODOMETER_STORE_KEY, meters);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "odometer_store_maybe_save: write failed (%s)", esp_err_to_name(err));
    } else {
        s_saved_meters = meters;
    }
    s_last_save_ms = now_ms;
    s_have_last_save = true;
}
