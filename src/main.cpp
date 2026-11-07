#include <stdio.h>
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
#include "App/Prefs.h"
#if DASH_PC_SCREEN
#include "App/PcScreen.h"
#endif
#if DASH_CAMERA
#include "App/CameraMode.h"
#endif
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
#if DASH_CAMERA
    // The camera, and the USB port's choice between the PC screen and the webcam (the user's last one). Before the
    // PC screen: the USB device presents whichever personality is preferred, whichever registers first.
    CameraMode_begin();
#endif
#if DASH_PC_SCREEN
    // The Windows USB screen on the high-speed port. Before the radio: the C6 bring-up below blocks for
    // seconds, and the PC should see the display as soon as it is plugged in.
    PcScreen_begin();
#endif

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
#if NM_NETWORK_ESPNOW
    // ESP-NOW through the C6 (dash-p4-c6 firmware): preferred for its latency -- the gateway fans ESP-NOW
    // traffic out locally before it reaches the broker. The library falls back to MQTT by itself while no
    // gateway has completed the handshake, and whenever the C6 runs a firmware without the bridge.
    NightMare::SelectConnection(NightMare::ConnectionType::ESP_NOW);
#else
    // MQTT is the only transport. The library starts the remote broker on the first
    // Wi-Fi connection (creds.h: REMOTE_MQTT_URL / PORT / ROOT_CA); this just pins the routing
    // preference so nothing else is ever chosen.
    NightMare::SelectConnection(NightMare::ConnectionType::MQTT);
#endif
    Heap_log("after network start");
    xTaskCreate(nmTask, "nm", 6144, nullptr, 3, nullptr);
}
