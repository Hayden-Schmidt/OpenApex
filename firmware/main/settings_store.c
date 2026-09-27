#include "settings_store.h"

#include "settings.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "settings_store";

#define SETTINGS_STORE_NAMESPACE "settings"
#define SETTINGS_STORE_KEY "tlv"
#define SETTINGS_STORE_QUIET_MS 1000U

static uint32_t s_saved_generation;
static uint32_t s_seen_generation;
static uint32_t s_seen_ms;

void settings_store_load(void) {
    terminal_settings_t s;
    settings_defaults(&s);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_STORE_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        uint8_t buf[64];
        size_t len = sizeof(buf);
        err = nvs_get_blob(handle, SETTINGS_STORE_KEY, buf, &len);
        nvs_close(handle);
        if (err == ESP_OK && !settings_tlv_decode(buf, len, &s)) {
            // A blob this firmware cannot read (e.g. a newer schema after a downgrade): defaults
            // beat a half-applied config, and the next phone write replaces it.
            ESP_LOGW(TAG, "settings_store_load: unreadable blob (%u bytes), using defaults",
                     (unsigned)len);
            settings_defaults(&s);
        }
    }
    if (err != ESP_OK) {
        // Expected on first-ever boot / a freshly erased partition.
        ESP_LOGI(TAG, "settings_store_load: no saved settings (%s)", esp_err_to_name(err));
    }
    settings_set(&s);
    s_saved_generation = s_seen_generation = settings_generation();
}

void settings_store_maybe_save(uint32_t now_ms) {
    const uint32_t gen = settings_generation();
    if (gen == s_saved_generation) return;
    if (gen != s_seen_generation) {
        s_seen_generation = gen;
        s_seen_ms = now_ms;
        return;
    }
    if ((now_ms - s_seen_ms) < SETTINGS_STORE_QUIET_MS) return;

    uint8_t buf[SETTINGS_TLV_MAX_LEN];
    const size_t len = settings_tlv_encode(settings_get(), buf, sizeof(buf));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, SETTINGS_STORE_KEY, buf, len);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        // Retried after the next quiet period rather than every tick.
        ESP_LOGW(TAG, "settings_store_maybe_save: write failed (%s)", esp_err_to_name(err));
        s_seen_ms = now_ms;
        return;
    }
    s_saved_generation = gen;
    ESP_LOGI(TAG, "settings saved");
}
