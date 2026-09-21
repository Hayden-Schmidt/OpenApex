idf_component_register(
    SRCS "main.c" "countdown.c" "packet.c" "normalize.c" "pipeline.c" "ble_link.c"
    INCLUDE_DIRS "."
    REQUIRES bt
)
