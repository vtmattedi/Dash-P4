#include <stdio.h>
#include <string>
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <NightMare.h>

#include "esp_hosted.h"

#include "board.h"
#include "App/App.h"
#include "App/Heap.h"
#include "App/MicTest.h"
#include "App/Net.h"
#include "App/Prefs.h"
#include "UI/Rest.h"
#include "UI/Ui.h"

static const char *TAG = "dash-p4";

// The framework's cooperative tick: Scheduler jobs, time sync, the serial console.
// Its own task, so a slow job can never stall drawing, and drawing never stalls it.
static void nmTask(void *)
{
    ESP_LOGI(TAG, "nm task running (serial console is polled here)");
    for (;;)
    {
        tickNightMareESP();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

#ifdef MIC_SELFTEST
// Temporary bring-up aid (build flag -DMIC_SELFTEST=1): the serial console does not take input on this board, so the
// microphone test runs by itself 20 s after boot and 20 s after that, logging the levels. Make some noise during it.
static void micSelfTest(void *)
{
    for (int round = 0; round < 3; ++round)
    {
        vTaskDelay(pdMS_TO_TICKS(20000));
        ESP_LOGW(TAG, "MIC SELFTEST: recording 5 s now, tap or talk next to the board");
        bool ok = false;
        const std::string report = MicTest_run(5, ok);
        ESP_LOGW(TAG, "MIC SELFTEST %s: %s", ok ? "ok" : "FAILED", report.c_str());
    }
    vTaskDelete(nullptr);
}
#endif

extern "C" void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    ESP_LOGI(TAG, "%s %s: chip rev v%d.%d, flash %u MB, psram %u MB",
             BOARD_NAME, BOARD_REVISION, chip.revision / 100, chip.revision % 100,
             (unsigned)(flash >> 20),
             (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) >> 20));

    // Display first: a panel that shows nothing while the radio comes up looks broken.
    ESP_ERROR_CHECK(board_init());
    if (board_lvgl_lock(0))
    {
        App_begin();
        Ui_begin(board_lvgl_display());
        Rest_begin();
        board_lvgl_unlock();
        ESP_LOGI(TAG, "UI built");
    }
    board_set_backlight(200);

    Heap_log("after board");
    // Opens the settings store and the device identity.
    introNightMareESP();
    App_loadSettings();

    // The Wi-Fi radio is the ESP32-C6 behind ESP-Hosted. esp_wifi_remote does NOT bring the
    // transport up by itself: without this, esp_wifi_init() fails with "Transport not
    // initialized, call esp_hosted_init() first" (seen on the first flash).
    // esp_hosted_connect_to_slave() waits for the C6 to answer over SDIO.
    if (esp_hosted_init() != 0 || esp_hosted_connect_to_slave() != 0)
    {
        ESP_LOGE(TAG, "ESP-Hosted link to the C6 failed; Wi-Fi will not start");
    }
    Heap_log("after hosted");

    board_set_backlight(static_cast<uint8_t>(Prefs_get().blActive));
    startNightMareESP();
    // Nothing network-wise has started by itself (NM_WIFI_AUTO=0, NM_ESPNOW_AUTO=0 in platformio.ini): the Wi-Fi station and the
    // one transport the user chose (Settings > Network) are brought up from the saved settings. ESP-NOW goes through the C6
    // (dash-p4-c6 firmware); with any other co-processor firmware it reports "not supported" and MQTT is the one to pick.
    Net_applyPrefs(true);
    Heap_log("after network start");
    xTaskCreate(nmTask, "nm", 6144, nullptr, 3, nullptr);
#ifdef MIC_SELFTEST
    xTaskCreate(micSelfTest, "micself", 6144, nullptr, 2, nullptr);
#endif
}
