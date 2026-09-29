#include "mesh_transport.h"

#include "board_profile.h"

#if BOARD_MESH_ESPNOW

#include "esp_event.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include <string.h>

// Rider mesh over ESP-NOW (docs/reference/rider_mesh_v1.md): broadcast-only on a fixed channel, STA mode, never associated.
// Needs ~35 KB of heap for the Wi-Fi stack, which is why the C3 leaves it out.

static const char *TAG = "mesh_espnow";

// Every terminal sits on this channel. Not connected to any AP, so nothing moves it.
#define MESH_CHANNEL 1

static const uint8_t kBroadcast[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static mesh_rx_fn s_rx;

// Wi-Fi task context.
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    (void)info;
    if (len > 0) s_rx(data, (size_t)len);
}

static bool espnow_start(mesh_rx_fn rx) {
    s_rx = rx;
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) goto fail;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&cfg)) != ESP_OK) goto fail;
    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) goto fail;
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) goto fail;
    if ((err = esp_wifi_start()) != ESP_OK) goto fail;
    if ((err = esp_wifi_set_channel(MESH_CHANNEL, WIFI_SECOND_CHAN_NONE)) != ESP_OK) goto fail;
    if ((err = esp_now_init()) != ESP_OK) goto fail;
    if ((err = esp_now_register_recv_cb(on_recv)) != ESP_OK) goto fail;
    esp_now_peer_info_t peer = {.channel = MESH_CHANNEL, .ifidx = WIFI_IF_STA, .encrypt = false};
    memcpy(peer.peer_addr, kBroadcast, sizeof(kBroadcast));
    if ((err = esp_now_add_peer(&peer)) != ESP_OK) goto fail;
    return true;
fail:
    ESP_LOGE(TAG, "init failed (%s)", esp_err_to_name(err));
    return false;
}

static void espnow_send(const uint8_t *frame, size_t len) {
    const esp_err_t err = esp_now_send(kBroadcast, frame, len);
    if (err != ESP_OK) ESP_LOGW(TAG, "send failed (%s)", esp_err_to_name(err));
}

const mesh_transport_t mesh_transport_espnow = {
    .name = "espnow",
    .start = espnow_start,
    .send = espnow_send,
    .poll = NULL,
};

#endif // BOARD_MESH_ESPNOW
