#include "display_driver.h"

#include "board_profile.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

#include "lvgl.h"

static const char *TAG = "display_driver";

// Partial-refresh draw buffers, BOARD_DISP_WIDTH wide x kBufLines tall (RGB565) -- DMA-capable so
// the SPI driver can transfer straight out of them without an extra copy.
static const int kBufLines = 40;

static esp_lcd_panel_handle_t s_panel;

// Runs in the SPI DMA ISR once a flush's color data has finished transferring.
static bool on_color_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata,
                                 void *user_ctx) {
    (void)io;
    (void)edata;
    lv_display_flush_ready((lv_display_t *)user_ctx);
    return false;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    (void)disp;
    // LVGL renders RGB565 as native little-endian uint16s; the GC9A01 clocks each pixel in
    // high-byte-first over SPI, so the two bytes have to be swapped before the transfer. Without
    // this the panel reads every pixel's halves transposed. Pure black (0x0000) and pure white
    // (0xFFFF) are byte-symmetric and survive unharmed, which is why the arrow itself still looked
    // right -- but red (0xF800) arrives as 0x00F8 (blue), and every anti-aliased grey edge pixel
    // lands on an unrelated near-black colour, which destroys the edge gradient and makes smooth
    // diagonals read as hard stair-steps.
    lv_draw_sw_rgb565_swap(px_map, lv_area_get_size(area));
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
}

void display_driver_init(void) {
    const gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << BOARD_DISP_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl_cfg));
    // Stays off until display_driver_backlight_on(). The panel's GRAM is uninitialized SRAM at
    // power-on, so anything lit before the first flush is noise.
    gpio_set_level(BOARD_DISP_PIN_BL, 0);

    const spi_bus_config_t bus_cfg = {
        .sclk_io_num = BOARD_DISP_PIN_SCLK,
        .mosi_io_num = BOARD_DISP_PIN_MOSI,
        .miso_io_num = -1, // 3-wire panel: no MISO line
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_DISP_WIDTH * kBufLines * (int)sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_DISP_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    lv_display_t *disp = lv_display_create(BOARD_DISP_WIDTH, BOARD_DISP_HEIGHT);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BOARD_DISP_PIN_CS,
        .dc_gpio_num = BOARD_DISP_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_trans_done,
        .user_ctx = disp,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_DISP_SPI_HOST, &io_cfg, &io));

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_DISP_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    const size_t buf_px = (size_t)BOARD_DISP_WIDTH * kBufLines;
    void *buf1 = heap_caps_malloc(buf_px * sizeof(uint16_t), MALLOC_CAP_DMA);
    void *buf2 = heap_caps_malloc(buf_px * sizeof(uint16_t), MALLOC_CAP_DMA);
    ESP_ERROR_CHECK((buf1 != NULL && buf2 != NULL) ? ESP_OK : ESP_ERR_NO_MEM);
    lv_display_set_buffers(disp, buf1, buf2, buf_px * sizeof(uint16_t),
                            LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_default(disp);

    ESP_LOGI(TAG, "GC9A01 %dx%d initialized (backlight off)", BOARD_DISP_WIDTH, BOARD_DISP_HEIGHT);
}

void display_driver_backlight_on(void) { gpio_set_level(BOARD_DISP_PIN_BL, 1); }
