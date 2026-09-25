idf_component_register(
    SRCS "main.c" "countdown.c" "packet.c" "normalize.c" "pipeline.c" "ble_link.c" "display_driver.c" "drive_log.c" "heading_fusion.c" "heading_store.c" "odometer.c" "odometer_store.c"
    INCLUDE_DIRS "."
    # app_update/esp_partition: the dev-only drive logger writes to its own flash partition. The
    # whole of drive_log.c compiles to nothing without -DOPENAPEX_DRIVE_LOG=1, so this costs a
    # production build only the (already-linked) component dependency.
    # nvs_flash: heading_store.c persists the learned mount offset/heading across power cycles
    # (docs/Heading_Sensor_Fusion_Plan.md) in the same NVS partition ble_link.c uses for BLE bonding.
    REQUIRES bt gui esp_lcd esp_driver_spi esp_driver_gpio esp_lcd_gc9a01 esp_partition nvs_flash
)
