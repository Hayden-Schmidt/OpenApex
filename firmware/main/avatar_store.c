#include "avatar_store.h"

#include "esp_log.h"
#include "host/ble_hs.h"
#include "nvs.h"

static const char *TAG = "avatar_store";

// Same namespace as settings_store.c: the avatar is one more rider setting, just too big for a TLV.
#define AVATAR_NAMESPACE "settings"
#define AVATAR_KEY "avatar"

static volatile bool s_changed;

// NVS is the only copy, so the GUI and BLE tasks never share a buffer.
bool avatar_store_read(uint8_t out[AVATAR_STORE_BYTES]) {
    nvs_handle_t handle;
    if (nvs_open(AVATAR_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t len = AVATAR_STORE_BYTES;
    const esp_err_t err = nvs_get_blob(handle, AVATAR_KEY, out, &len);
    nvs_close(handle);
    return err == ESP_OK && len == AVATAR_STORE_BYTES;
}

bool avatar_store_take_changed(void) {
    if (!s_changed) return false;
    s_changed = false;
    return true;
}

static int write(const uint8_t *buf, uint16_t len) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(AVATAR_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = len == 0 ? nvs_erase_key(handle, AVATAR_KEY) : nvs_set_blob(handle, AVATAR_KEY, buf, len);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK; // clearing an empty store
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "avatar write failed (%s)", esp_err_to_name(err));
        return BLE_ATT_ERR_UNLIKELY;
    }
    s_changed = true;
    ESP_LOGI(TAG, "avatar %s", len == 0 ? "cleared" : "saved");
    return 0;
}

int avatar_store_ble_access(struct ble_gatt_access_ctxt *ctxt) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t buf[AVATAR_STORE_BYTES];
        if (!avatar_store_read(buf)) return 0;
        return os_mbuf_append(ctxt->om, buf, sizeof(buf)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    // Longer than the default MTU, so the phone sends it as a long write; NimBLE reassembles the
    // prepared writes and hands over the whole value here, so a torn upload never lands.
    uint8_t buf[AVATAR_STORE_BYTES];
    uint16_t len = 0;
    if (OS_MBUF_PKTLEN(ctxt->om) > sizeof(buf) ||
        ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0 ||
        (len != 0 && len != AVATAR_STORE_BYTES)) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    return write(buf, len);
}
