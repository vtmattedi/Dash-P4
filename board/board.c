#include "board.h"
#include "esp_check.h"

static const char *TAG = "board";

esp_err_t board_init(void)
{
    static bool done;
    if (done) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(board_i2c_bus_init(), TAG, "i2c");
    ESP_RETURN_ON_ERROR(board_display_init(), TAG, "display");
    ESP_RETURN_ON_ERROR(board_touch_init(), TAG, "touch");
    ESP_RETURN_ON_ERROR(board_lvgl_start(), TAG, "lvgl");
    done = true;
    return ESP_OK;
}
