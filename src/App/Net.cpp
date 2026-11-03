#include "Net.h"

#include <atomic>
#include <map>
#include <mutex>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "net";

namespace
{
constexpr size_t kInboxDepth = 256;
constexpr size_t kStatusDepth = 32;
constexpr size_t kMaxListeners = 8;

// One allocation per message: header, topic + NUL, payload + NUL.
struct Item
{
    uint32_t topicLen;
    uint32_t payloadLen;
    uint32_t receivedMs;
    bool retained;
    // char topic[topicLen + 1]; uint8_t payload[payloadLen + 1];
    char *topic() { return reinterpret_cast<char *>(this + 1); }
    uint8_t *payload() { return reinterpret_cast<uint8_t *>(topic() + topicLen + 1); }
};

QueueHandle_t g_inbox;
QueueHandle_t g_statusQueue;

std::atomic<uint32_t> g_received{0}, g_delivered{0}, g_dropped{0}, g_bytes{0}, g_connects{0};
std::atomic<uint32_t> g_highWater{0};
std::atomic<bool> g_trace{false};
std::atomic<bool> g_connectedPending{false};

NetMessageFn g_messageListeners[kMaxListeners];
NetStatusFn g_statusListeners[kMaxListeners];
NetConnectedFn g_connectedListeners[kMaxListeners];

// Subscription table: filter -> reference count. Touched from the LVGL task
// (Net_subscribe) and from the library's task (the OnConnect replay).
std::mutex g_subMutex;
std::map<std::string, int> g_subscriptions;

template <typename Fn>
bool addListener(Fn (&table)[kMaxListeners], Fn fn)
{
    for (size_t i = 0; i < kMaxListeners; ++i)
    {
        if (table[i] == fn)
        {
            return true;
        }
    }
    for (size_t i = 0; i < kMaxListeners; ++i)
    {
        if (table[i] == nullptr)
        {
            table[i] = fn;
            return true;
        }
    }
    return false;
}

void *allocBig(size_t bytes)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
}

// Library task. Copy and queue; never block, never touch app state.
void onLibraryMessage(const char *topic, const uint8_t *payload, size_t length, bool retained)
{
    g_received.fetch_add(1, std::memory_order_relaxed);
    if (!g_inbox || !topic || length > NET_MAX_PAYLOAD)
    {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const size_t topicLen = strlen(topic);
    Item *item = static_cast<Item *>(allocBig(sizeof(Item) + topicLen + 1 + length + 1));
    if (!item)
    {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    item->topicLen = static_cast<uint32_t>(topicLen);
    item->payloadLen = static_cast<uint32_t>(length);
    item->receivedMs = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    item->retained = retained;
    memcpy(item->topic(), topic, topicLen + 1);
    if (length)
    {
        memcpy(item->payload(), payload, length);
    }
    item->payload()[length] = 0;

    if (xQueueSend(g_inbox, &item, 0) != pdTRUE)
    {
        heap_caps_free(item);
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_bytes.fetch_add(static_cast<uint32_t>(length), std::memory_order_relaxed);
    const uint32_t depth = uxQueueMessagesWaiting(g_inbox);
    uint32_t high = g_highWater.load(std::memory_order_relaxed);
    while (depth > high && !g_highWater.compare_exchange_weak(high, depth))
    {
    }
}

void onLibraryStatus(const NightMare::NetworkStatusEvent &event)
{
    if (!g_statusQueue)
    {
        return;
    }
    NetStatus status{event.layer, event.state, event.previous, event.up};
    xQueueSend(g_statusQueue, &status, 0);
}

// Library task, on every (re)connect of the active transport.
void onLibraryConnected()
{
    std::vector<std::string> filters;
    {
        std::lock_guard<std::mutex> lock(g_subMutex);
        filters.reserve(g_subscriptions.size());
        for (const auto &entry : g_subscriptions)
        {
            filters.push_back(entry.first);
        }
    }
    for (const std::string &filter : filters)
    {
        NightMare::Subscribe(filter.c_str());
    }
    g_connects.fetch_add(1, std::memory_order_relaxed);
    g_connectedPending.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "connected: replayed %u subscription(s)", (unsigned)filters.size());
}
} // namespace

bool Net_onMessage(NetMessageFn fn) { return addListener(g_messageListeners, fn); }
bool Net_onStatus(NetStatusFn fn) { return addListener(g_statusListeners, fn); }
bool Net_onConnected(NetConnectedFn fn) { return addListener(g_connectedListeners, fn); }

void Net_subscribe(const std::string &filter)
{
    if (filter.empty())
    {
        return;
    }
    bool first;
    {
        std::lock_guard<std::mutex> lock(g_subMutex);
        first = (g_subscriptions[filter]++ == 0);
    }
    if (first)
    {
        // Harmless when no transport is up: the replay on connect installs it.
        NightMare::Subscribe(filter.c_str());
    }
}

void Net_unsubscribe(const std::string &filter)
{
    bool last = false;
    {
        std::lock_guard<std::mutex> lock(g_subMutex);
        auto it = g_subscriptions.find(filter);
        if (it == g_subscriptions.end())
        {
            return;
        }
        if (--it->second <= 0)
        {
            g_subscriptions.erase(it);
            last = true;
        }
    }
    if (last)
    {
        NightMare::Unsubscribe(filter.c_str());
    }
}

bool Net_publish(const std::string &topic, const std::string &payload, bool retained)
{
    return NightMare::PublishText(topic, payload, retained);
}

bool Net_publishBytes(const std::string &topic, const uint8_t *payload, size_t length, bool retained)
{
    return NightMare::Publish(topic.c_str(), payload, length, retained);
}

void Net_begin()
{
    if (!g_inbox)
    {
        g_inbox = xQueueCreate(kInboxDepth, sizeof(Item *));
        g_statusQueue = xQueueCreate(kStatusDepth, sizeof(NetStatus));
    }

    // What the panel watches. These are standing: replayed on every reconnect.
    //   +/status                   who is out there, and whether it is up (retained)
    //   +/info                     boot-scoped device description (retained)
    //   +/telemetry/system|network runtime health (retained)
    //   +/manifest/msgpack         what each device exposes (retained)
    //   +/resource/+/state         every Value's current state (retained)
    //   Control/forecast           the weather, answered by the backend
    // Per-device extras (events, console replies, raw tail) are subscribed on demand.
    {
        std::lock_guard<std::mutex> lock(g_subMutex);
        for (const char *filter : {"+/status", "+/info", "+/telemetry/system", "+/telemetry/network",
                                   "+/manifest/msgpack", "+/resource/+/state", "Control/forecast"})
        {
            g_subscriptions[filter] += 1;
        }
    }

    NightMare::OnMessage(onLibraryMessage);
    NightMare::OnConnect(onLibraryConnected);
    NightMare::OnNetworkStatus(onLibraryStatus);
}

void Net_drain(uint32_t budgetMs)
{
    if (!g_inbox)
    {
        return;
    }
    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(budgetMs) * 1000;

    NetStatus status;
    while (xQueueReceive(g_statusQueue, &status, 0) == pdTRUE)
    {
        ESP_LOGI(TAG, "layer %s: %s -> %s", NightMare::NetworkLayerName(status.layer),
                 NightMare::ConnectivityStateName(status.previous),
                 NightMare::ConnectivityStateName(status.state));
        for (NetStatusFn fn : g_statusListeners)
        {
            if (fn)
            {
                fn(status);
            }
        }
    }

    if (g_connectedPending.exchange(false, std::memory_order_acq_rel))
    {
        for (NetConnectedFn fn : g_connectedListeners)
        {
            if (fn)
            {
                fn();
            }
        }
    }

    Item *item;
    while (xQueueReceive(g_inbox, &item, 0) == pdTRUE)
    {
        const NetMessage message{item->topic(), item->payload(), item->payloadLen, item->retained,
                                 item->receivedMs};
        if (g_trace.load(std::memory_order_relaxed))
        {
            ESP_LOGI(TAG, "%s%s (%u B)", message.retained ? "R " : "  ", message.topic,
                     (unsigned)message.length);
        }
        for (NetMessageFn fn : g_messageListeners)
        {
            if (fn)
            {
                fn(message);
            }
        }
        heap_caps_free(item);
        g_delivered.fetch_add(1, std::memory_order_relaxed);
        if (esp_timer_get_time() >= deadline)
        {
            break;
        }
    }
}

NetState Net_state()
{
    NetState state{};
    state.active = NightMare::GetActiveConnection();
    state.preferred = NightMare::GetPreferredConnection();
    state.connection = NightMare::GetConnectionState();
    state.wifiIp = NightMare::WiFiIP_state() == NightMare::ConnectivityState::CONNECTED;
    state.mqtt = NightMare::Mqtt_state() == NightMare::ConnectivityState::CONNECTED;
    state.espNow = false;
#if NM_NETWORK_ESPNOW
    state.espNow = NightMare::EspNow_state() == NightMare::ConnectivityState::CONNECTED;
#endif
    return state;
}

NetStats Net_stats()
{
    return {g_received.load(), g_delivered.load(), g_dropped.load(),
            g_bytes.load(),    g_connects.load(),  g_highWater.load()};
}

void Net_setTrace(bool on) { g_trace.store(on); }
bool Net_trace() { return g_trace.load(); }
