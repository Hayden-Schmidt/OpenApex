#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// CST816 capacitive touch over I2C, registered with LVGL as a pointer input device. Only built
// for boards with BOARD_HAS_TOUCH; call after display_driver_init(), since the input device
// attaches to the default display.
void touch_driver_init(void);

#ifdef __cplusplus
}
#endif
