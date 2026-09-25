#include "touch_driver.h"

#include "board_profile.h"

// The S3 profile declares touch (a CST9217) but has no pins yet, so only boards with a pin map
// build the driver.
#if BOARD_HAS_TOUCH && defined(BOARD_TOUCH_PIN_SDA)

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

#include "lvgl.h"

static const char *TAG = "touch_driver";

#define CST816_ADDR 0x15
#define CST816_REG_FINGER_NUM 0x02  // then XH, XL, YH, YL
#define CST816_REG_DIS_AUTO_SLEEP 0xFE
// Short enough that a read the chip does not answer never stalls the GUI frame.
#define CST816_TIMEOUT_MS 10

static i2c_master_dev_handle_t s_dev;

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    static int32_t last_x;
    static int32_t last_y;
    uint8_t reg = CST816_REG_FINGER_NUM;
    uint8_t buf[5];
    // A NACK is normal, not an error: the chip does not answer while it is asleep, and asleep
    // means no finger.
    if (i2c_master_transmit_receive(s_dev, &reg, 1, buf, sizeof(buf), CST816_TIMEOUT_MS) != ESP_OK ||
        buf[0] == 0) {
        data->state = LV_INDEV_STATE_RELEASED;
    } else {
        last_x = ((buf[1] & 0x0F) << 8) | buf[2];
        last_y = ((buf[3] & 0x0F) << 8) | buf[4];
        if (last_x >= BOARD_DISP_WIDTH) last_x = BOARD_DISP_WIDTH - 1;
        if (last_y >= BOARD_DISP_HEIGHT) last_y = BOARD_DISP_HEIGHT - 1;
        data->state = LV_INDEV_STATE_PRESSED;
    }
    data->point.x = last_x;
    data->point.y = last_y;
}

void touch_driver_init(void) {
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = BOARD_TOUCH_PIN_SDA,
        .scl_io_num = BOARD_TOUCH_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CST816_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev));
    // Sleeping-chip NACKs are expected on every idle poll; keep them out of the log.
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    // Best effort: keeps the chip awake so the first touch after a quiet spell is not lost to
    // wake-up. Fails harmlessly if it is already asleep.
    const uint8_t no_sleep[2] = {CST816_REG_DIS_AUTO_SLEEP, 0x01};
    const bool awake = i2c_master_transmit(s_dev, no_sleep, sizeof(no_sleep), CST816_TIMEOUT_MS) == ESP_OK;

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, read_cb);
    // The nav page's outer-ring toggle is a deliberate 2 s press-and-hold (see DialScreen).
    lv_indev_set_long_press_time(indev, 2000);

    ESP_LOGI(TAG, "CST816 touch registered (auto-sleep %s)", awake ? "disabled" : "unchanged, chip asleep");
}

#else

void touch_driver_init(void) {}

#endif
