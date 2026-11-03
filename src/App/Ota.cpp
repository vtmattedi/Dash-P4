#include "Ota.h"

#include <atomic>
#include <string.h>

#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_hosted.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";

namespace
{
std::atomic<OtaState> g_state{OtaState::Idle};
std::atomic<int> g_percent{-1};
std::atomic<bool> g_coprocessor{false};
std::string g_message;      // written by the OTA task, read by the UI: guarded by the state flag
std::string g_url;

// ESP-Hosted's own example writes 1400-byte chunks; each is one RPC to the C6.
constexpr int kCoprocessorChunk = 1400;

void setMessage(const std::string &text) { g_message = text; }

void otaTask(void *)
{
    esp_http_client_config_t http = {};
    http.url = g_url.c_str();
    http.timeout_ms = 15000;
    http.keep_alive_enable = true;
    http.crt_bundle_attach = esp_crt_bundle_attach; // https:// verified against the Mozilla bundle

    esp_https_ota_config_t config = {};
    config.http_config = &http;

    esp_https_ota_handle_t handle = nullptr;
    esp_err_t err = esp_https_ota_begin(&config, &handle);
    if (err != ESP_OK)
    {
        setMessage(std::string("Could not start: ") + esp_err_to_name(err));
        g_state = OtaState::Failed;
        vTaskDelete(nullptr);
        return;
    }
    setMessage("Downloading...");

    while (true)
    {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS)
        {
            break;
        }
        const int total = esp_https_ota_get_image_size(handle);
        const int read = esp_https_ota_get_image_len_read(handle);
        g_percent = total > 0 ? static_cast<int>(static_cast<int64_t>(read) * 100 / total) : -1;
    }

    if (err == ESP_OK && esp_https_ota_is_complete_data_received(handle))
    {
        err = esp_https_ota_finish(handle); // validates the image and switches the boot partition
        if (err == ESP_OK)
        {
            g_percent = 100;
            setMessage("Update written. Restart to run it.");
            g_state = OtaState::Done;
            ESP_LOGI(TAG, "update complete");
            vTaskDelete(nullptr);
            return;
        }
        setMessage(std::string("The image was rejected: ") + esp_err_to_name(err));
    }
    else
    {
        esp_https_ota_abort(handle);
        setMessage(std::string("Download failed: ") + esp_err_to_name(err));
    }
    g_state = OtaState::Failed;
    ESP_LOGW(TAG, "%s", g_message.c_str());
    vTaskDelete(nullptr);
}

void failCoprocessor(const std::string &text, bool started)
{
    if (started)
    {
        esp_hosted_slave_ota_end(); // closes the C6's session; the half-written slot is never booted
    }
    setMessage(text);
    g_state = OtaState::Failed;
    ESP_LOGW(TAG, "C6: %s", text.c_str());
}

// The C6's image is checked here before a byte reaches it: a P4 image (or a web page) in its OTA slot would
// leave the board without Wi-Fi until the C6 is reflashed by hand.
bool isCoprocessorImage(const uint8_t *data, int len, std::string &why)
{
    if (len < static_cast<int>(sizeof(esp_image_header_t)))
    {
        why = "The download is too short to be a firmware image.";
        return false;
    }
    esp_image_header_t header;
    memcpy(&header, data, sizeof(header));
    if (header.magic != ESP_IMAGE_HEADER_MAGIC)
    {
        why = "That is not an ESP firmware image (use the .bin from the dash-p4-c6 build, not a factory image).";
        return false;
    }
    if (header.chip_id != ESP_CHIP_ID_ESP32C6)
    {
        why = "That image is for another chip (id " + std::to_string(header.chip_id) + "), not the ESP32-C6.";
        return false;
    }
    return true;
}

void coprocessorTask(void *)
{
    esp_http_client_config_t http = {};
    http.url = g_url.c_str();
    http.timeout_ms = 15000;
    http.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&http);
    if (!client)
    {
        failCoprocessor("Could not create the HTTP client.", false);
        vTaskDelete(nullptr);
        return;
    }
    esp_err_t err = esp_http_client_open(client, 0);
    const int64_t total = err == ESP_OK ? esp_http_client_fetch_headers(client) : -1;
    const int status = esp_http_client_get_status_code(client);
    if (err != ESP_OK || status != 200)
    {
        esp_http_client_cleanup(client);
        failCoprocessor(err != ESP_OK ? std::string("Could not connect: ") + esp_err_to_name(err)
                                      : "The server answered HTTP " + std::to_string(status) + ".",
                        false);
        vTaskDelete(nullptr);
        return;
    }

    static uint8_t chunk[kCoprocessorChunk];
    bool started = false;
    int64_t written = 0;
    setMessage("Downloading to the C6...");
    for (;;)
    {
        const int n = esp_http_client_read(client, reinterpret_cast<char *>(chunk), sizeof(chunk));
        if (n < 0)
        {
            esp_http_client_cleanup(client);
            failCoprocessor("Download failed.", started);
            vTaskDelete(nullptr);
            return;
        }
        if (n == 0)
        {
            break;
        }
        if (!started)
        {
            std::string why;
            if (!isCoprocessorImage(chunk, n, why))
            {
                esp_http_client_cleanup(client);
                failCoprocessor(why, false);
                vTaskDelete(nullptr);
                return;
            }
            err = esp_hosted_slave_ota_begin();
            if (err != ESP_OK)
            {
                esp_http_client_cleanup(client);
                failCoprocessor(std::string("The C6 refused to start an update: ") + esp_err_to_name(err), false);
                vTaskDelete(nullptr);
                return;
            }
            started = true;
        }
        err = esp_hosted_slave_ota_write(chunk, static_cast<uint32_t>(n));
        if (err != ESP_OK)
        {
            esp_http_client_cleanup(client);
            failCoprocessor(std::string("Writing to the C6 failed: ") + esp_err_to_name(err), started);
            vTaskDelete(nullptr);
            return;
        }
        written += n;
        g_percent = total > 0 ? static_cast<int>(written * 100 / total) : -1;
    }
    esp_http_client_cleanup(client);
    if (!started || (total > 0 && written != total))
    {
        failCoprocessor("The download ended early.", started);
        vTaskDelete(nullptr);
        return;
    }
    err = esp_hosted_slave_ota_end(); // the C6 validates the image
    if (err != ESP_OK)
    {
        failCoprocessor(std::string("The C6 rejected the image: ") + esp_err_to_name(err), false);
        vTaskDelete(nullptr);
        return;
    }
    g_percent = 100;
    setMessage("C6 firmware written. Restart to run it.");
    g_state = OtaState::Done;
    ESP_LOGI(TAG, "C6 update complete (%lld bytes)", static_cast<long long>(written));
    vTaskDelete(nullptr);
}

bool startTask(const std::string &url, bool coprocessor)
{
    if (url.empty() || g_state == OtaState::Running)
    {
        return false;
    }
    g_url = url;
    g_percent = -1;
    g_coprocessor = coprocessor;
    setMessage("Starting...");
    g_state = OtaState::Running;
    if (xTaskCreate(coprocessor ? coprocessorTask : otaTask, "ota", 10 * 1024, nullptr, 3, nullptr) != pdPASS)
    {
        setMessage("Could not create the update task.");
        g_state = OtaState::Failed;
        return false;
    }
    return true;
}
} // namespace

bool Ota_start(const std::string &url) { return startTask(url, false); }

bool Ota_startCoprocessor(const std::string &url) { return startTask(url, true); }

bool Ota_isCoprocessor() { return g_coprocessor; }

void Ota_restartAfterCoprocessor()
{
    // Slaves before 2.6 do not know this call and switched slots at the end of the write already.
    const esp_err_t err = esp_hosted_slave_ota_activate();
    ESP_LOGI(TAG, "C6 activate: %s; restarting", esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

OtaState Ota_state() { return g_state; }
int Ota_percent() { return g_percent; }
std::string Ota_message() { return g_message; }

void Ota_markValid()
{
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED)
    {
        ESP_LOGW(TAG, "mark valid: %s", esp_err_to_name(err));
    }
}
