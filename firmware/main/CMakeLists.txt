idf_component_register(
    SRCS "main.c" "countdown.c" "packet.c" "normalize.c" "pipeline.c" "ble_link.c" "ancs_core.c" "ancs_nav.c" "ancs_bridge.c" "apple_client.c" "cts_client.c" "display_driver.c" "touch_driver.c" "drive_log.c" "heading_fusion.c" "heading_store.c" "odometer.c" "odometer_store.c" "call_packet.c" "call_state.c" "settings.c" "settings_store.c" "avatar_store.c" "mesh.c" "mesh_transport_ble.c" "mesh_transport_espnow.c" "mesh_core.c" "mesh_proto.c" "mesh_crypto.c"
    INCLUDE_DIRS "."
    # app_update/esp_partition: the dev-only drive logger writes to its own flash partition. The
    # whole of drive_log.c compiles to nothing without -DOPENAPEX_DRIVE_LOG=1, so this costs a
    # production build only the (already-linked) component dependency.
    # nvs_flash: heading_store.c persists the learned mount offset/heading across power cycles
    # (docs/Heading_Sensor_Fusion_Plan.md) in the same NVS partition ble_link.c uses for BLE bonding.
    # nvs_flash also backs settings_store.c and avatar_store.c.
    # esp_wifi/esp_event: the rider mesh's ESP-NOW transport, linked only on boards with BOARD_MESH_ESPNOW.
    REQUIRES bt gui esp_lcd esp_driver_spi esp_driver_gpio esp_driver_i2c esp_lcd_gc9a01 esp_partition nvs_flash esp_wifi esp_event
)
