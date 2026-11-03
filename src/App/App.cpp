#include "App.h"

#include <NightMare.h>

#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "AcClient.h"
#include "Bindings.h"
#include "Heap.h"
#include "LightControl.h"
#include "Mqttp.h"
#include "Net.h"
#include "Notifications.h"
#include "Ota.h"
#include "Prefs.h"
#include "Registry.h"
#include "Explorer/Ping.h"
#include "Explorer/TopicTap.h"
#include "Explorer/Watch.h"
#if DASH_PC_SCREEN
#include "PcScreen.h"
#endif

static const char *TAG = "app";

namespace
{
// After the panel (re)connects the broker replays every retained status. Those are
// "the state of the world", not events, so they must not become notifications.
constexpr uint32_t kReplayQuietMs = 6000;
uint32_t g_quietUntilMs = 0;

uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// --- Network messages: the app's one dispatch point -------------------------------

void onMessage(const NetMessage &message)
{
    Tap_record(message); // the raw tail sees everything, including what Mqttp consumes
    if (Mqttp_handleMessage(message))
    {
        return;
    }
    Registry_handle(message);
}

// --- Transport health -> notifications --------------------------------------------

void onStatus(const NetStatus &status)
{
    using NightMare::ConnectivityState;
    using NightMare::NetworkLayer;

    const char *key = nullptr;
    const char *name = nullptr;
    switch (status.layer)
    {
    case NetworkLayer::WIFI_IP:
        key = "net.wifi";
        name = "Wi-Fi";
        break;
    case NetworkLayer::MQTT:
        key = "net.mqtt";
        name = "MQTT";
        break;
    case NetworkLayer::ESP_NOW:
        key = "net.espnow";
        name = "ESP-NOW";
        break;
    default:
        return; // the radio itself is not interesting to the user
    }

    if (status.up)
    {
        NotifyClear(key, std::string(name) + " connected");
    }
    else if (status.state == ConnectivityState::ERROR)
    {
        NotifySticky(key, NotifySeverity::Crit, "network", std::string(name) + " error");
    }
    else if (status.state == ConnectivityState::STOPPED || status.state == ConnectivityState::CONNECTING ||
             status.state == ConnectivityState::STARTING || status.state == ConnectivityState::SUSPENDED)
    {
        // Booting up is not a fault; a layer that WAS up and went away is.
        if (status.previous == ConnectivityState::CONNECTED)
        {
            NotifySticky(key, NotifySeverity::Warn, "network", std::string(name) + " lost",
                         "Reconnecting...");
        }
    }
}

void onConnected()
{
    g_quietUntilMs = nowMs() + kReplayQuietMs;
}

// --- Devices coming and going -> notifications ------------------------------------

void onPresence(const Device &device, bool online, bool firstSeen)
{
    if (device.kind == "gateway")
    {
        const std::string key = "gw." + device.name;
        if (online)
        {
            NotifyClear(key);
        }
        else if (!firstSeen)
        {
            NotifySticky(key, NotifySeverity::Warn, "gateway:" + device.name, "Gateway " + device.name + " is offline");
        }
        return;
    }
    if (static_cast<int32_t>(nowMs() - g_quietUntilMs) < 0)
    {
        return; // retained replay
    }
    if (firstSeen && !online)
    {
        return; // an old retained "offline" for something we never saw alive
    }
    if (online)
    {
        Notify(NotifySeverity::Ok, "device:" + device.name, device.name + " is online");
    }
    else
    {
        Notify(NotifySeverity::Warn, "device:" + device.name, device.name + " went offline");
    }
}

// Every state publication of a watched resource becomes a history sample.
void onValue(const std::string &device, const std::string &resource, const ResourceState &state)
{
    if (!state.withdrawn)
    {
        Watch_record(device, resource, state.value, state.lastSeenMs);
    }
}

bool publishPort(const std::string &topic, const std::string &payload)
{
    return Net_publish(topic, payload, false);
}
void subscribePort(const std::string &filter) { Net_subscribe(filter); }
void unsubscribePort(const std::string &filter) { Net_unsubscribe(filter); }
} // namespace

#pragma region "Console"

// Commands this firmware adds to the framework's built-ins (PING, INFO, CONFIG, JOB,
// MQTT, WIFI, REBOOT, STATUS, `>` resource commands ...). The resolver runs on whichever
// task received the command -- the serial console, or the library's MQTT task -- so app
// state is only read between board_lvgl_lock() and board_lvgl_unlock().
//
//   HEAP                       internal / PSRAM free, largest internal block
//   NET                        transport snapshot and inbox counters
//   NETLOG [ON|OFF]            print every received topic to the serial log
//   REGISTRY [<device>]        counts, or one device's resources
//   NOTIFY <INFO|OK|WARN|CRIT> <title...>   raise a test notification
//   TARGET                     the bindings as JSON
//   TARGET <AC|LIGHT|RGB|DOOR|REST> <device> [resource]   bind a role ("" unbinds)
//   PCSCREEN [SHOW|HIDE|CLOSE|AUTO [ON|OFF]]              the Windows USB screen (PC-screen builds only)
//   C6OTA <url> | STATUS | RESTART                        update the ESP32-C6 co-processor over ESP-Hosted

static NightMareResults answer(bool ok, const std::string &text)
{
    NightMareResults res;
    res.result = ok;
    res.response = text;
    return res;
}

static NightMareResults resolveAppCommand(const NightMareMessage &message)
{
    char line[360];

    if (message.command == "HEAP")
    {
        snprintf(line, sizeof(line),
                 "{\"internalFree\":%u,\"internalLargest\":%u,\"internalMinFree\":%u,"
                 "\"psramFree\":%u,\"psramTotal\":%u}",
                 (unsigned)Heap_internalFree(), (unsigned)Heap_internalLargest(),
                 (unsigned)Heap_internalMinFree(), (unsigned)Heap_psramFree(),
                 (unsigned)Heap_psramTotal());
        return answer(true, line);
    }

    if (message.command == "NET")
    {
        const NetState state = Net_state();
        const NetStats stats = Net_stats();
        snprintf(line, sizeof(line),
                 "{\"active\":\"%s\",\"preferred\":\"%s\",\"wifiIp\":%s,\"mqtt\":%s,\"espNow\":%s,"
                 "\"received\":%u,\"delivered\":%u,\"dropped\":%u,\"bytes\":%u,\"connects\":%u,"
                 "\"inboxHighWater\":%u}",
                 NightMare::ConnectionTypeName(state.active),
                 NightMare::ConnectionTypeName(state.preferred), state.wifiIp ? "true" : "false",
                 state.mqtt ? "true" : "false", state.espNow ? "true" : "false",
                 (unsigned)stats.received, (unsigned)stats.delivered, (unsigned)stats.dropped,
                 (unsigned)stats.bytes, (unsigned)stats.connects, (unsigned)stats.inboxHighWater);
        return answer(true, line);
    }

    if (message.command == "NETLOG")
    {
        if (message.subcommand == "ON")
        {
            Net_setTrace(true);
        }
        else if (message.subcommand == "OFF")
        {
            Net_setTrace(false);
        }
        return answer(true, Net_trace() ? "netlog on" : "netlog off");
    }

    if (message.command == "REGISTRY")
    {
        if (!board_lvgl_lock(500))
        {
            return answer(false, "UI busy, try again");
        }
        std::string out;
        if (message.argc >= 1 && !message.args[0].empty())
        {
            const Device *d = Registry_find(message.args[0]);
            if (!d)
            {
                board_lvgl_unlock();
                return answer(false, "unknown device (names are case-sensitive)");
            }
            out = "{\"name\":\"" + d->name + "\",\"online\":" + (d->online ? "true" : "false") +
                  ",\"hardware\":\"" + d->hardware + "\",\"manifestBytes\":" +
                  std::to_string(d->manifest.size()) + ",\"resources\":{";
            bool first = true;
            for (const auto &entry : d->resources)
            {
                out += std::string(first ? "" : ",") + "\"" + entry.first + "\":\"" +
                       (entry.second.withdrawn ? "(withdrawn)" : entry.second.value.substr(0, 40)) + "\"";
                first = false;
            }
            out += "}}";
        }
        else
        {
            snprintf(line, sizeof(line),
                     "{\"devices\":%u,\"online\":%u,\"resources\":%u,\"evictedDevices\":%u,"
                     "\"evictedResources\":%u,\"maxDevices\":%d,\"maxResources\":%d}",
                     (unsigned)Registry_deviceCount(), (unsigned)Registry_onlineCount(),
                     (unsigned)Registry_resourceCount(), (unsigned)Registry_evictedDevices(),
                     (unsigned)Registry_evictedResources(), REGISTRY_MAX_DEVICES, REGISTRY_MAX_RESOURCES);
            out = line;
        }
        board_lvgl_unlock();
        return answer(true, out);
    }

    if (message.command == "TARGET")
    {
        if (!board_lvgl_lock(500))
        {
            return answer(false, "UI busy, try again");
        }
        std::string out;
        bool ok = true;
        if (message.subcommand.empty())
        {
            out = Bindings_json();
        }
        else
        {
            int role = -1;
            for (int r = 0; r < static_cast<int>(Role::Count); ++r)
            {
                std::string label = Bindings_label(static_cast<Role>(r));
                for (char &c : label)
                {
                    c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
                }
                if (message.subcommand == label || (r == 4 && message.subcommand == "REST"))
                {
                    role = r;
                }
            }
            if (role < 0)
            {
                ok = false;
                out = "Unknown role. Expected one of: AC, LIGHT, RGB, DOOR, REST.";
            }
            else
            {
                // args[0] is the role (the subcommand); the device and resource follow it.
                Bindings_set(static_cast<Role>(role), message.argc >= 2 ? message.args[1] : std::string(),
                             message.argc >= 3 ? message.args[2] : std::string());
                out = Bindings_json();
            }
        }
        board_lvgl_unlock();
        return answer(ok, out);
    }

    if (message.command == "C6OTA")
    {
        if (message.subcommand == "STATUS")
        {
            const OtaState state = Ota_state();
            const char *name = state == OtaState::Running ? "running"
                               : state == OtaState::Done  ? "done"
                               : state == OtaState::Failed ? "failed"
                                                           : "idle";
            return answer(true, std::string("{\"target\":\"") + (Ota_isCoprocessor() ? "c6" : "p4") + "\",\"state\":\"" +
                                    name + "\",\"percent\":" + std::to_string(Ota_percent()) + ",\"message\":\"" +
                                    Ota_message() + "\"}");
        }
        if (message.subcommand == "RESTART")
        {
            if (Ota_state() != OtaState::Done || !Ota_isCoprocessor())
            {
                return answer(false, "no finished C6 update to run");
            }
            Ota_restartAfterCoprocessor();
            return answer(true, "restarting"); // not reached
        }
        // The URL keeps its case: args[0] is the raw first word (the subcommand is its upper-cased copy).
        const std::string url = message.argc >= 1 ? message.args[0] : std::string();
        if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
        {
            return answer(false, "usage: C6OTA <http(s) URL of an ESP32-C6 firmware.bin> | C6OTA STATUS | C6OTA RESTART");
        }
        if (!Ota_startCoprocessor(url))
        {
            return answer(false, "an update is already running");
        }
        return answer(true, "C6 update started; C6OTA STATUS to follow it");
    }

    if (message.command == "NOTIFY")
    {
        NotifySeverity severity = NotifySeverity::Info;
        if (message.subcommand == "OK")
        {
            severity = NotifySeverity::Ok;
        }
        else if (message.subcommand == "WARN")
        {
            severity = NotifySeverity::Warn;
        }
        else if (message.subcommand == "CRIT")
        {
            severity = NotifySeverity::Crit;
        }
        else if (message.subcommand != "INFO")
        {
            return answer(false, "usage: NOTIFY <INFO|OK|WARN|CRIT> <title> [body]");
        }
        // args[0] is the severity (the subcommand); the title and body follow it.
        const std::string title = message.argc >= 2 && !message.args[1].empty() ? message.args[1] : "Test notification";
        NotifyAsync(severity, "console", title, message.argc >= 3 ? message.args[2] : std::string());
        return answer(true, "queued");
    }

#if DASH_PC_SCREEN
    if (message.command == "PCSCREEN")
    {
        return PcScreen_command(message);
    }
    return answer(false, "Unknown command. Available: [HEAP, NET, NETLOG, REGISTRY, NOTIFY, TARGET, C6OTA, PCSCREEN]. "
                         "Framework built-ins and `>` resource commands still apply.");
#else
    return answer(false, "Unknown command. Available: [HEAP, NET, NETLOG, REGISTRY, NOTIFY, TARGET, C6OTA]. "
                         "Framework built-ins and `>` resource commands still apply.");
#endif
}

#pragma endregion

static bool g_settingsLoaded = false;

static void housekeepingTimer(lv_timer_t *)
{
    static uint32_t appliedRevision = UINT32_MAX;
    static uint32_t lastMirrorMs = 0;

    Mqttp_tick();
    Ping_tick();
    Notifications_tick();

    // While bringing the display up: say whether LVGL is actually completing frames.
    static uint32_t lastReportMs = 0;
    static int reports = 0;
    if (reports < 6 && nowMs() - lastReportMs >= 3000)
    {
        lastReportMs = nowMs();
        ++reports;
        ESP_LOGI(TAG, "lvgl frames=%u backlight=%u", (unsigned)board_lvgl_frames(), (unsigned)board_get_backlight());
    }

    if (!g_settingsLoaded)
    {
        return;
    }
    // Re-point the bound resources whenever a binding moved.
    if (appliedRevision != Bindings_revision())
    {
        appliedRevision = Bindings_revision();
        AcClient_applyBindings();
        LightControl_applyBindings();
    }
    // The library consumes the state of bound resources before it reaches the app; mirror
    // them back so the Explorer still sees them.
    if (nowMs() - lastMirrorMs >= 1000)
    {
        lastMirrorMs = nowMs();
        AcClient_mirrorToRegistry();
        LightControl_mirrorToRegistry();
    }
}

static void drainTimer(lv_timer_t *)
{
    Net_drain(8);
}

void App_begin()
{
    Heap_log("app begin");
    setCommandResolver(resolveAppCommand);

    const MqttpPorts ports{publishPort, subscribePort, unsubscribePort, nowMs};
    Mqttp_setPorts(ports);

    Net_onMessage(onMessage);
    Net_onStatus(onStatus);
    Net_onConnected(onConnected);
    Registry_onPresence(onPresence);
    Registry_onValue(onValue);
    Net_begin();

    // 20 ms: fast enough that a tap on a live value feels live, slow enough that a
    // retained-state replay (hundreds of messages) is spread over many frames.
    lv_timer_create(drainTimer, 20, nullptr);
    lv_timer_create(housekeepingTimer, 100, nullptr);

    // A freshly updated image is only kept if it is still running 20 s after boot.
    lv_timer_t *confirm = lv_timer_create([](lv_timer_t *) { Ota_markValid(); }, 20000, nullptr);
    lv_timer_set_repeat_count(confirm, 1);

    // Standing conditions, true from the start and cleared as the network comes up.
    NotifySticky("net.wifi", NotifySeverity::Warn, "network", "Wi-Fi not connected");
    NotifySticky("net.mqtt", NotifySeverity::Warn, "network", "MQTT not connected");
    ESP_LOGI(TAG, "ready");
}

void App_loadSettings()
{
    if (!board_lvgl_lock(0))
    {
        return;
    }
    Prefs_load();
    Bindings_load();
    Watch_restore(PersistentSettings.get("explorer_watch", ""));
    // Declared before the framework starts: a pending identity cleanup can only withdraw
    // resources that already exist, and the resource manager installs each subscription as
    // soon as it knows the address.
    AcClient_begin();
    LightControl_begin();
    g_settingsLoaded = true;
    board_lvgl_unlock();
}
