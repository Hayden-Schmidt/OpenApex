idf_component_register(
    SRCS "main.c" "countdown.c" "packet.c" "normalize.c" "pipeline.c" "ble_link.c" "display_driver.c" "drive_log.c"
    INCLUDE_DIRS "."
    # app_update/esp_partition: the dev-only drive logger writes to its own flash partition. The
    # whole of drive_log.c compiles to nothing without -DOPENAPEX_DRIVE_LOG=1, so this costs a
    # production build only the (already-linked) component dependency.
    REQUIRES bt gui esp_lcd esp_driver_spi esp_driver_gpio esp_lcd_gc9a01 esp_partition
)
