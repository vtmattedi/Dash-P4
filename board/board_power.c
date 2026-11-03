#include "board_power.h"
#include "board_pins.h"
#include "esp_check.h"
#include "esp_ldo_regulator.h"

static const char *TAG = "board_power";

esp_err_t board_power_dsi_phy(void)
{
    static esp_ldo_channel_handle_t phy = NULL;
    if (phy) {
        return ESP_OK;
    }
    const esp_ldo_channel_config_t cfg = {
        .chan_id = BOARD_LCD_DSI_PHY_LDO_CHAN,
        .voltage_mv = BOARD_LCD_DSI_PHY_LDO_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&cfg, &phy), TAG, "DSI PHY LDO");
    return ESP_OK;
}
