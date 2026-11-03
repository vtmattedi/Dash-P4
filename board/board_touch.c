#include "board_touch.h"
#include "board_pins.h"

#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"

static const char *TAG = "board_touch";

static i2c_master_bus_handle_t s_bus;
static esp_lcd_touch_handle_t s_touch;

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return s_bus;
}

esp_err_t board_i2c_bus_init(void)
{
    if (s_bus) {
        return ESP_OK;
    }
    const i2c_master_bus_config_t cfg = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA_GPIO,
        .scl_io_num = BOARD_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_bus);
}

esp_lcd_touch_handle_t board_touch(void)
{
    return s_touch;
}

esp_err_t board_touch_init(void)
{
    if (s_touch) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(board_i2c_bus_init(), TAG, "i2c bus");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.dev_addr = BOARD_TOUCH_I2C_ADDR;
    io_cfg.scl_speed_hz = BOARD_I2C_FREQ_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_bus, &io_cfg, &io), TAG, "touch io");

    const esp_lcd_touch_config_t cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = BOARD_TOUCH_RESET_GPIO,
        .int_gpio_num = BOARD_TOUCH_INT_GPIO,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(io, &cfg, &s_touch), TAG, "gt911");
    ESP_LOGI(TAG, "GT911 at 0x%02X on I2C%d (SDA %d, SCL %d)", BOARD_TOUCH_I2C_ADDR,
             BOARD_I2C_PORT, BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO);
    return ESP_OK;
}
