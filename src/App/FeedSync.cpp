#include "FeedSync.h"

#include <atomic>
#include <mutex>

#include <NightMare.h>

#include "Feed.h"
#include "Net.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "creds.h"

#ifndef NOTIF_API_TOKEN
#define NOTIF_API_TOKEN ""
#endif
#ifndef NOTIF_BASE_URL
#define NOTIF_BASE_URL "https://notif.mattediworks.com"
#endif

static const char *TAG = "feed";

namespace
{
// The background poll is slow on purpose: the saved cache makes power-up instant, "Sync now" makes a refresh instant, and
// a wall calendar does not need to be fresher than a few minutes. (Was 30 s before the cache existed.)
constexpr uint32_t kPollMs = 5 * 60 * 1000;
constexpr uint32_t kRetryMs = 60000;      // after any failure
constexpr uint32_t kFirstPollMs = 8000;   // let Wi-Fi come up
constexpr size_t kMaxBody = 512 * 1024;   // a runaway answer must not eat the PSRAM
constexpr const char *kTokenKey = "notif_token";

std::mutex g_mutex; // guards the strings and the status below
std::string g_token;
std::string g_baseUrl = NOTIF_BASE_URL;
FeedSyncStatus g_status;
int64_t g_okAtMs = -1;

TaskHandle_t g_task = nullptr;
std::atomic<bool> g_resync{false};

int64_t nowMs() { return esp_timer_get_time() / 1000; }

void report(const std::string &message, int http)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status.message = message;
    g_status.lastHttp = http;
    if (message == "ok")
    {
        g_okAtMs = nowMs();
    }
}

std::string token()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_token;
}

enum class Outcome
{
    Ok,
    Reset, // 409: the cache is for another stream, start over at once
    Failed
};

// One request. `version`/`epoch` is the cursor; on Ok it has been advanced and the delta posted.
Outcome pollOnce(int64_t &version, std::string &epoch)
{
    std::string url;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        url = g_baseUrl;
    }
    url += "/api/v1/feed?version=" + std::to_string(version);
    if (version > 0 && !epoch.empty())
    {
        url += "&epoch=" + epoch;
    }
    const std::string bearer = "Bearer " + token();

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = 15000;
    config.crt_bundle_attach = esp_crt_bundle_attach; // https:// verified against the Mozilla bundle
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
    {
        report("could not create the HTTP client", 0);
        return Outcome::Failed;
    }
    esp_http_client_set_header(client, "Authorization", bearer.c_str());
    esp_http_client_set_header(client, "Accept", "application/json");

    Outcome outcome = Outcome::Failed;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
        report(std::string("could not connect: ") + esp_err_to_name(err), 0);
    }
    else
    {
        esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        std::string body;
        char chunk[1024];
        bool truncated = false;
        while (true)
        {
            const int n = esp_http_client_read(client, chunk, sizeof(chunk));
            if (n < 0)
            {
                truncated = true;
                break;
            }
            if (n == 0)
            {
                break;
            }
            body.append(chunk, static_cast<size_t>(n));
            if (body.size() > kMaxBody)
            {
                truncated = true;
                break;
            }
        }

        if (status == 409)
        {
            report("reset required", status);
            outcome = Outcome::Reset;
        }
        else if (status != 200)
        {
            report("HTTP " + std::to_string(status), status);
        }
        else if (truncated)
        {
            report("the response was cut short", status);
        }
        else
        {
            FeedDelta delta;
            if (!Feed_parse(body.data(), body.size(), delta))
            {
                report("malformed response", status);
            }
            else
            {
                // A reply to version=0 is the whole feed whatever `full` says: a patch cannot apply to nothing.
                if (version == 0)
                {
                    delta.full = true;
                }
                version = delta.version;
                if (!delta.epoch.empty())
                {
                    epoch = delta.epoch;
                }
                Feed_post(std::move(delta));
                report("ok", status);
                outcome = Outcome::Ok;
            }
        }
    }
    esp_http_client_cleanup(client);
    return outcome;
}

int64_t g_startVersion = 0; // the cache's cursor at start-up (FeedSync_begin copies it on the LVGL task)
std::string g_startEpoch;

void feedTask(void *)
{
    int64_t version = g_startVersion;
    std::string epoch = g_startEpoch;
    uint32_t waitMs = kFirstPollMs;

    while (true)
    {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(waitMs)); // a notification means "poll now"

        if (g_resync.exchange(false))
        {
            version = 0;
            epoch.clear();
        }
        if (token().empty())
        {
            report("no token", 0);
            waitMs = kRetryMs;
            continue;
        }
        // Not before Wi-Fi has an address: the TCP/IP stack does not exist until the network task has started it, and a
        // DNS lookup before that asserts inside lwIP ("Invalid mbox"). Look again shortly instead of backing off.
        if (!Net_state().wifiIp)
        {
            waitMs = 2000;
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            ++g_status.polls;
        }
        Outcome outcome = pollOnce(version, epoch);
        if (outcome == Outcome::Reset)
        {
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                ++g_status.resets;
            }
            version = 0;
            epoch.clear();
            ESP_LOGW(TAG, "stream reset, resyncing");
            outcome = pollOnce(version, epoch);
        }
        if (outcome != Outcome::Ok)
        {
            ESP_LOGW(TAG, "poll failed");
        }
        waitMs = outcome == Outcome::Ok ? kPollMs : kRetryMs;
    }
}
} // namespace

void FeedSync_begin()
{
    g_startVersion = Feed_version();
    g_startEpoch = Feed_epoch();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_token = PersistentSettings.get(kTokenKey, "");
        if (g_token.empty())
        {
            g_token = NOTIF_API_TOKEN;
        }
    }
    // TLS handshake plus a JSON body: 10 KB of stack. The stack lives in PSRAM: internal RAM is the scarce resource
    // (the PC-screen + camera build has ~57 KB free once Wi-Fi is up, and ESP-Hosted asserts when its SDIO receive
    // buffer cannot be allocated). This task never writes flash itself (dismissals are saved by the LVGL task), which
    // is what a PSRAM stack must not do.
    if (xTaskCreateWithCaps(feedTask, "feed", 10240, nullptr, 2, &g_task, MALLOC_CAP_SPIRAM) != pdPASS)
    {
        xTaskCreate(feedTask, "feed", 10240, nullptr, 2, &g_task);
    }
}

void FeedSync_setToken(const std::string &newToken)
{
    PersistentSettings.set(kTokenKey, newToken);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_token = newToken.empty() ? std::string(NOTIF_API_TOKEN) : newToken;
    }
    FeedSync_resync();
}

void FeedSync_pollNow()
{
    if (g_task)
    {
        xTaskNotifyGive(g_task);
    }
}

void FeedSync_resync()
{
    g_resync = true;
    FeedSync_pollNow();
}

FeedSyncStatus FeedSync_status()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    FeedSyncStatus copy = g_status;
    copy.hasToken = !g_token.empty();
    copy.lastOkAgoMs = g_okAtMs < 0 ? UINT32_MAX : static_cast<uint32_t>(nowMs() - g_okAtMs);
    return copy;
}
