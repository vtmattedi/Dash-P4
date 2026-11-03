// Stage-1 firmware (env jc1060p470c_usbscreen): the panel as a Windows USB extended display, nothing else.
// No radio, no NightMare. The panel shows a status screen until the PC streams, then the PC's display.

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "board.h"
#include "PcLink/PcLink.h"
#include "StatusScreen.h"

static const char *TAG = "usb-screen";

extern "C" void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    ESP_LOGI(TAG, "%s %s: chip rev v%d.%d, flash %u MB, psram %u MB", BOARD_NAME, BOARD_REVISION,
             chip.revision / 100, chip.revision % 100, (unsigned)(flash >> 20),
             (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) >> 20));

    ESP_ERROR_CHECK(board_init());
    if (board_lvgl_lock(0))
    {
        PcLinkOptions options;
        ESP_ERROR_CHECK(PcLink_begin(options));
        StatusScreen_begin();
        board_lvgl_unlock();
    }
    board_set_backlight(200);
    ESP_LOGI(TAG, "internal heap %u KB free, PSRAM %u KB free",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10));
}
