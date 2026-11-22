#include "Net.h"
#include "Diag.h"

#include "Prefs.h"

#include <atomic>
#include <map>
#include <mutex>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "net";

namespace
{
constexpr size_t kMaxListeners = 8;

std::atomic<uint32_t> g_oversize{0}, g_bytes{0}, g_connects{0};
std::atomic<bool> g_trace{false};

NetMessageFn g_messageListeners[kMaxListeners];
NetStatusFn g_statusListeners[kMaxListeners];
NetConnectedFn g_connectedListeners[kMaxListeners];

// Subscription table: filter -> reference count. The library keeps the filters
// themselves and reinstalls them on every (re)connect; this only counts users.
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

// The library queues these (NM_MESSAGE_QUEUE_DEPTH) and Net_drain() runs them
// on the LVGL task through NightMare::DeliverEvents().
void onLibraryMessage(const NightMare::ReceivedMessage &received)
{
    if (received.length > NET_MAX_PAYLOAD)
    {
        g_oversize.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_bytes.fetch_add(static_cast<uint32_t>(received.length), std::memory_order_relaxed);
    const NetMessage message{received.topic, received.payload, static_cast<uint32_t>(received.length),
                             received.retained, static_cast<uint32_t>(esp_timer_get_time() / 1000)};
    if (g_trace.load(std::memory_order_relaxed))
    {
        ESP_LOGI(TAG, "%s%s (%u B)", message.retained ? "R " : "  ", message.topic, (unsigned)message.length);
    }
    for (NetMessageFn fn : g_messageListeners)
    {
        if (fn)
        {
            fn(message);
        }
    }
}

void onLibraryStatus(const NightMare::NetworkStatusEvent &event)
{
    ESP_LOGI(TAG, "layer %s: %s -> %s", NightMare::NetworkLayerName(event.layer),
             NightMare::ConnectivityStateName(event.previous), NightMare::ConnectivityStateName(event.state));
    const NetStatus status{event.layer, event.state, event.previous, event.up};
    for (NetStatusFn fn : g_statusListeners)
    {
        if (fn)
        {
            fn(status);
        }
    }
}

// Every (re)connect of the transport, after the library reinstalled the filters.
void onLibraryConnected()
{
    g_connects.fetch_add(1, std::memory_order_relaxed);
    for (NetConnectedFn fn : g_connectedListeners)
    {
        if (fn)
        {
            fn();
        }
    }
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
        // Kept by the library: installed now if a transport is up, and on every reconnect.
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
    // What the panel watches, kept by the library for every (re)connect:
    //   +/status                   who is out there, and whether it is up (retained)
    //   +/info                     boot-scoped device description (retained)
    //   +/telemetry/system|network runtime health (retained)
    //   +/manifest/msgpack         what each device exposes (retained)
    //   +/resource/+/state         every Value's current state (retained)
    //   +/gateway/network|clients  a gateway's radio, uplinks and ESP-NOW clients (retained; the drawer shows them)
    //   Control/forecast           the weather, answered by the backend
    // Per-device extras (events, console replies, raw tail) are subscribed on demand.
    for (const char *filter : {"+/status", "+/info", "+/telemetry/system", "+/telemetry/network", "+/manifest/msgpack",
                               "+/resource/+/state", "+/gateway/network", "+/gateway/clients", "Control/forecast"})
    {
        Net_subscribe(filter);
    }

    NightMare::OnMessage(onLibraryMessage);
    NightMare::OnConnect(onLibraryConnected);
    NightMare::OnNetworkStatus(onLibraryStatus);
}

void Net_drain(uint32_t budgetMs)
{
    NightMare::DeliverEvents(budgetMs);
}

namespace
{
std::atomic<bool> g_applying{false};
std::atomic<bool> g_applyAgain{false};

void applyNow()
{
    Diag_mark("network apply");
    const Prefs &prefs = Prefs_get();

    // The Wi-Fi station is core: always up. MQTT and ESP-NOW are independent switches. Only one carries traffic at a time (ESP-NOW
    // while the gateway session is up, MQTT otherwise), so with both on the broker topics (Stackport, weather) keep arriving
    // if the gateway is lost.
    const bool wantEspNow = prefs.espNowOn;
    const bool wantMqtt = prefs.mqttOn;

    if (!wantMqtt && NightMare::Mqtt_enabled())
    {
        NightMare::Mqtt_disable();
    }
    if (!NightMare::WiFiIP_enabled() && !NightMare::WiFiBegin())
    {
        ESP_LOGE(TAG, "Wi-Fi station did not start");
    }
#if NM_NETWORK_ESPNOW
    if (!wantEspNow && NightMare::EspNow_enabled())
    {
        NightMare::EspNow_disable();
    }
    if (wantEspNow && !NightMare::EspNow_enabled() && !NightMare::EspNow_enable())
    {
        ESP_LOGE(TAG, "ESP-NOW did not start");
    }
#endif
    if (wantMqtt && !NightMare::Mqtt_enabled())
    {
        // The library's saved profile (REMOTE unless changed with NETWORK MQTT ENABLE LOCAL).
        if (!NightMare::Mqtt_enable())
        {
            ESP_LOGE(TAG, "MQTT did not start");
        }
    }
    if (wantEspNow)
    {
        NightMare::SelectConnection(NightMare::ConnectionType::ESP_NOW); // preferred; MQTT is the fallback when it is also on
    }
    else if (wantMqtt)
    {
        NightMare::SelectConnection(NightMare::ConnectionType::MQTT);
    }
}

// Switching transports talks to the C6 (RPC round trips, each able to wait seconds) and starts or stops tasks, so from the
// UI it runs on its own short-lived task: the LVGL task keeps drawing. A change made while it runs is applied after.
void applyTask(void *)
{
    do
    {
        g_applyAgain = false;
        applyNow();
    } while (g_applyAgain.load());
    g_applying = false;
    vTaskDelete(nullptr);
}
} // namespace

void Net_applyPrefs(bool wait)
{
    if (wait)
    {
        applyNow();
        return;
    }
    if (g_applying.exchange(true))
    {
        g_applyAgain = true;
        return;
    }
    if (xTaskCreate(applyTask, "netapply", 6144, nullptr, 3, nullptr) != pdPASS)
    {
        g_applying = false;
        applyNow(); // no memory for a task: do it here rather than not at all
    }
}

bool Net_applying() { return g_applying.load(); }

NetState Net_state()
{
    NetState state{};
    state.active = NightMare::GetActiveConnection();
    state.preferred = NightMare::GetPreferredConnection();
    state.connection = NightMare::GetConnectionState();
    state.wifiIp = NightMare::WiFiIP_state() == NightMare::ConnectivityState::CONNECTED;
    state.mqtt = NightMare::Mqtt_state() == NightMare::ConnectivityState::CONNECTED;
    state.espNow = false;
    state.espNowEnabled = false;
#if NM_NETWORK_ESPNOW
    state.espNowEnabled = NightMare::EspNow_enabled();
    state.espNow = NightMare::EspNow_state() == NightMare::ConnectivityState::CONNECTED;
#endif
    return state;
}

NetStats Net_stats()
{
    const NightMare::DeliveryStats delivery = NightMare::GetDeliveryStats();
    return {delivery.queued + delivery.dropped, delivery.delivered, delivery.dropped + g_oversize.load(),
            g_bytes.load(), g_connects.load(), delivery.highWater};
}

void Net_setTrace(bool on) { g_trace.store(on); }
bool Net_trace() { return g_trace.load(); }
