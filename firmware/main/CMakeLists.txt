idf_component_register(
    SRCS "main.c" "countdown.c" "packet.c" "normalize.c" "pipeline.c" "ble_link.c" "display_driver.c"
    INCLUDE_DIRS "."
    REQUIRES bt gui esp_lcd esp_driver_spi esp_driver_gpio esp_lcd_gc9a01
)
