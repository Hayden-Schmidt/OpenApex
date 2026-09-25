#include "heading_store.h"

#include "heading_fusion.h"

#include "esp_log.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "heading_store";

#define HEADING_STORE_NAMESPACE "heading"
#define HEADING_STORE_KEY "state"
#define HEADING_STORE_SAVE_INTERVAL_MS (30U * 1000U)

static uint32_t s_last_save_ms;
static bool s_have_last_save;

void heading_store_load(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(HEADING_STORE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        // Expected on first-ever boot / a freshly erased partition -- nothing cached yet.
        ESP_LOGI(TAG, "heading_store_load: no cached state (%s)", esp_err_to_name(err));
        return;
    }

    heading_fusion_state_t saved;
    size_t len = sizeof(saved);
    err = nvs_get_blob(handle, HEADING_STORE_KEY, &saved, &len);
    nvs_close(handle);

    if (err != ESP_OK || len != sizeof(saved)) {
        // A size mismatch means the on-flash blob is from a different heading_fusion_state_t
        // layout (firmware upgrade) -- discard rather than feed a partially-overlaid struct in.
        ESP_LOGI(TAG, "heading_store_load: no usable cached state (%s)", esp_err_to_name(err));
        return;
    }

    heading_fusion_restore_state(&saved);
    ESP_LOGI(TAG, "heading_store_load: restored cached heading/offset");
}

void heading_store_maybe_save(uint32_t now_ms) {
    if (s_have_last_save && (now_ms - s_last_save_ms) < HEADING_STORE_SAVE_INTERVAL_MS) {
        return;
    }

    heading_fusion_state_t state;
    heading_fusion_get_state(&state);
    if (!state.have_filtered) {
        // Nothing learned yet this boot -- avoid overwriting a perfectly good prior cache with an
        // empty one before the first fix arrives.
        return;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(HEADING_STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "heading_store_maybe_save: nvs_open failed (%s)", esp_err_to_name(err));
        return;
    }
    err = nvs_set_blob(handle, HEADING_STORE_KEY, &state, sizeof(state));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "heading_store_maybe_save: write failed (%s)", esp_err_to_name(err));
    }

    s_last_save_ms = now_ms;
    s_have_last_save = true;
}
