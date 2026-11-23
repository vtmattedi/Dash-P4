#include "App.h"

#include <NightMare.h>
#include <Core/Storage.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <algorithm>
#include <math.h>
#include <string>

#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "AcClient.h"
#include "LedFeedback.h"
#include "MicTest.h"
#include "LedStrip.h"
#include "PcPower.h"
#include "SysMon.h"
#include "Feed.h"
#include "Link/LinkServer.h"
#include "UI/Rest.h"
#include "UI/Ui.h"
#include "FeedSync.h"
#include "Bindings.h"
#include "Heap.h"
#include "LightControl.h"
#include "Mqttp.h"
#include "Net.h"
#include "Ota.h"
#include "Prefs.h"
#include "Stackport.h"
#include "StackportSim.h"
#include "Registry.h"
#include "Explorer/Ping.h"
#include "Explorer/TopicTap.h"
#include "Explorer/Watch.h"

static const char *TAG = "app";

namespace
{
uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// --- Network messages: the app's one dispatch point -------------------------------

void onMessage(const NetMessage &message)
{
    Tap_record(message); // the raw tail sees everything, including what Mqttp consumes
    if (Mqttp_handleMessage(message) || Stackport_handle(message))
    {
        return;
    }
    Registry_handle(message);
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
//   LED <r> <g> <b> | <RRGGBB> | OFF | BRIGHT <0-64> | STATUS   the WS2812B strip
//   NET                        transport snapshot and inbox counters
//   NETLOG [ON|OFF]            print every received topic to the serial log
//   REGISTRY [<device>]        counts, or one device's resources
//   TARGET                     the bindings as JSON
//   TARGET <AC|LIGHT|RGB|DOOR|REST> <device> [resource]   bind a role ("" unbinds)
//   PCSCREEN [SHOW|HIDE|CLOSE|AUTO [ON|OFF]]              the Windows USB screen (PC-screen builds only)
//   CAMERA [SHOW|HOME|SHARE ON|OFF|SET <name> <value>|RESET]   the board's camera (camera builds only)
//   PERF [ON|OFF]                     LVGL FPS / CPU / render-time readout on the panel
//   LINK [TOKEN <token>|REGEN|DROP]   the PC link: status, pairing token, drop the linked PC
//   FEED [SYNC|RESET|CLEAR|TOKEN <token>]   the notif.mattediworks.com feed: status, poll now, resync, un-dismiss, set token
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
    char line[640];

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

    if (message.command == "LED")
    {
        if (!LedStrip_ready())
        {
            return answer(false, "LED strip not available");
        }
        bool ok = true;
        // The console is one more feedback source ("console", above the built-in network one, below alerts); OFF/CLEAR drops it.
        if (message.subcommand == "OFF" || message.subcommand == "CLEAR")
        {
            LedFeedback_clear("console");
        }
        else if (message.subcommand == "BRIGHT" && message.argc >= 2)
        {
            ok = LedStrip_setBrightness(static_cast<uint8_t>(atoi(message.args[1].c_str())));
        }
        else if (message.argc >= 3) // LED <r> <g> <b>
        {
            const uint32_t r = atoi(message.args[0].c_str()) & 0xFF;
            const uint32_t g = atoi(message.args[1].c_str()) & 0xFF;
            const uint32_t b = atoi(message.args[2].c_str()) & 0xFF;
            LedEffect fx;
            fx.rgb = (r << 16) | (g << 8) | b;
            ok = LedFeedback_set("console", fx, LED_PRIO_INFO);
        }
        else if (message.argc >= 1 && message.subcommand != "STATUS") // LED RRGGBB (hex, optional #)
        {
            const char *hex = message.args[0].c_str();
            LedEffect fx;
            fx.rgb = static_cast<uint32_t>(strtoul(hex[0] == '#' ? hex + 1 : hex, nullptr, 16));
            ok = LedFeedback_set("console", fx, LED_PRIO_INFO);
        }
        snprintf(line, sizeof(line), "{\"ok\":%s,\"colour\":\"%06X\",\"brightness\":%u}", ok ? "true" : "false",
                 (unsigned)LedStrip_colour(), (unsigned)LedStrip_brightness());
        return answer(ok, line);
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
                if (message.subcommand == label || (r == 1 && message.subcommand == "LIGHT") || (r == 2 && message.subcommand == "RGB") ||
                    (r == 4 && message.subcommand == "REST"))
                {
                    role = r;
                }
            }
            if (role < 0)
            {
                ok = false;
                out = "Unknown role. Expected one of: AC, LIGHT (light and door), BEDLED (RGB), DOOR, REST.";
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

    if (message.command == "MIC")
    {
        // MIC [seconds [gain dB]]: record from the board microphone and report levels (a test of the codec, the pins and the
        // gain; the recording is thrown away). Runs on the console task, not the LVGL one.
        const int seconds = message.argc >= 1 ? atoi(message.args[0].c_str()) : 3;
        if (message.argc >= 2)
        {
            board_audio_set_gain(static_cast<float>(atof(message.args[1].c_str())));
        }
        bool ok = false;
        const std::string report = MicTest_run(seconds, ok);
        return answer(ok, report);
    }

    if (message.command == "SCREENPASS")
    {
        // Panel recovery for faint edge/column ghosts (image sticking): full-screen solid colours cycled every 2 s on the
        // system layer, above everything; any touch or SCREENPASS STOP ends it. Same recovery that cleared the ghosts in
        // .local/findings.md section 16 (about half an hour). Optional argument: minutes (default 30).
        static lv_obj_t *cover = nullptr;
        static lv_timer_t *timer = nullptr;
        static int step = 0, remaining = 0;
        if (!board_lvgl_lock(1000))
        {
            return answer(false, "LVGL busy");
        }
        auto stop = []()
        {
            if (timer)
            {
                lv_timer_delete(timer);
                timer = nullptr;
            }
            if (cover)
            {
                lv_obj_delete(cover);
                cover = nullptr;
            }
        };
        std::string out;
        if (message.subcommand == "STOP")
        {
            stop();
            out = "screen pass stopped";
        }
        else
        {
            stop();
            const int minutes = message.argc >= 1 ? atoi(message.args[0].c_str()) : 30;
            remaining = (minutes > 0 ? minutes : 30) * 30; // ticks of 2 s
            step = 0;
            cover = lv_obj_create(lv_layer_sys());
            lv_obj_remove_style_all(cover);
            lv_obj_set_size(cover, LV_PCT(100), LV_PCT(100));
            lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
            lv_obj_add_flag(cover, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(cover, [](lv_event_t *) { /* touch ends it */
                                    if (timer) { lv_timer_delete(timer); timer = nullptr; }
                                    if (cover) { lv_obj_delete_async(cover); cover = nullptr; } },
                                LV_EVENT_CLICKED, nullptr);
            timer = lv_timer_create([](lv_timer_t *)
                                    {
                static const uint32_t kColours[] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF, 0x000000};
                if (!cover) return;
                lv_obj_set_style_bg_color(cover, lv_color_hex(kColours[step++ % 5]), 0);
                if (--remaining <= 0) { lv_timer_delete(timer); timer = nullptr; lv_obj_delete_async(cover); cover = nullptr; } },
                                    2000, nullptr);
            lv_timer_ready(timer);
            out = "screen pass running; touch the screen or SCREENPASS STOP to end it";
        }
        board_lvgl_unlock();
        return answer(true, out);
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

    if (message.command == "FEED")
    {
        if (!board_lvgl_lock(500))
        {
            return answer(false, "UI busy, try again");
        }
        if (message.subcommand == "SYNC")
        {
            FeedSync_pollNow();
        }
        else if (message.subcommand == "RESET")
        {
            FeedSync_resync();
        }
        else if (message.subcommand == "CLEAR")
        {
            Feed_clear(); // empties the cache; the next poll refills it, dismissed items stay dismissed
            FeedSync_resync();
        }
        else if (message.subcommand == "TOKEN")
        {
            // args[0] is the word TOKEN; the token follows. Empty falls back to creds.h.
            FeedSync_setToken(message.argc >= 2 ? message.args[1] : std::string());
        }
        const FeedSyncStatus sync = FeedSync_status();
        snprintf(line, sizeof(line),
                 "{\"token\":%s,\"message\":\"%s\",\"http\":%d,\"okAgoMs\":%d,\"polls\":%u,\"resets\":%u,"
                 "\"version\":%lld,\"items\":%u,\"visible\":%u}",
                 sync.hasToken ? "true" : "false", sync.message.c_str(), sync.lastHttp,
                 sync.lastOkAgoMs == UINT32_MAX ? -1 : (int)sync.lastOkAgoMs, (unsigned)sync.polls,
                 (unsigned)sync.resets, (long long)Feed_version(), (unsigned)Feed_total(),
                 (unsigned)Feed_visibleCount());
        board_lvgl_unlock();
        return answer(true, line);
    }

    if (message.command == "SPSIM")
    {
        // SPSIM <down|warn|run|all|offline|OFF>: show the rest screen what Stackport would look like (App/StackportSim.h).
        // OFF forgets it and fetches the real retained state again.
        if (!board_lvgl_lock(500))
        {
            return answer(false, "UI busy, try again");
        }
        std::string what = message.subcommand;
        for (char &c : what)
        {
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        }
        bool ok = true;
        if (what == "off")
        {
            StackportSim_stop();
            Net_unsubscribe(kStackportStatusFilter, true);
            Net_unsubscribe(kStackportStateFilter, true);
            Net_subscribe(kStackportStatusFilter, true);
            Net_subscribe(kStackportStateFilter, true);
#ifdef STACKPORT_BOOT_SIM
    StackportSim_start(STACKPORT_BOOT_SIM, true); // a build with -DSTACKPORT_BOOT_SIM=\"all\" shows the fake Stackport from boot
#endif
        }
        else
        {
            ok = StackportSim_start(what.c_str());
        }
        board_lvgl_unlock();
        return answer(ok, ok ? (what == "off" ? "simulation off, real state requested" : "simulating") : "SPSIM down|warn|run|all|offline|off");
    }

    if (message.command == "PERF")
    {
        // PERF [ON|OFF]: LVGL's own FPS / CPU / render-time readout (bottom right); also Settings > Display > Show FPS.
        if (!board_lvgl_lock(500))
        {
            return answer(false, "UI busy, try again");
        }
        const bool on = message.subcommand == "ON" || (message.subcommand != "OFF" && !Ui_perfShown());
        Ui_setPerf(on);
        board_lvgl_unlock();
        return answer(true, on ? "perf monitor on" : "perf monitor off");
    }

    if (message.command == "LINK")
    {
        // LINK [TOKEN <token>|REGEN|DROP]: the PC link (pc/PROTOCOL.md): status, pairing token, drop the linked PC.
        if (message.subcommand == "TOKEN" || message.subcommand == "REGEN")
        {
            if (!board_lvgl_lock(500))
            {
                return answer(false, "UI busy, try again");
            }
            const bool ok = LinkServer_setToken(message.subcommand == "REGEN" ? std::string()
                                                                              : (message.argc >= 2 ? message.args[1] : std::string()));
            board_lvgl_unlock();
            if (!ok)
            {
                return answer(false, "a token is 4 to 32 printable characters without spaces");
            }
        }
        else if (message.subcommand == "DROP")
        {
            LinkServer_disconnect();
        }
        const LinkStatus link = LinkServer_status();
        snprintf(line, sizeof(line),
                 "{\"listening\":%s,\"connected\":%s,\"ip\":\"%s\",\"port\":%u,\"token\":\"%s\",\"peer\":\"%s\","
                 "\"os\":\"%s\",\"caps\":\"%s\",\"heardAgoMs\":%u,\"sessions\":%u,\"rejected\":%u,\"window\":\"%.80s\","
                 "\"message\":\"%s\"}",
                 link.listening ? "true" : "false", link.connected ? "true" : "false", link.ip.c_str(),
                 (unsigned)link.port, LinkServer_token().c_str(), link.peer.c_str(), link.peerOs.c_str(),
                 link.caps.c_str(), (unsigned)link.heardAgoMs, (unsigned)link.sessions, (unsigned)link.rejected,
                 link.window.c_str(), link.message.c_str());
        return answer(true, line);
    }

    return answer(false, "Unknown command. Available: [HEAP, LED, NET, NETLOG, REGISTRY, FEED, LINK, PERF, SPSIM, TARGET, C6OTA, SCREENPASS, MIC]. "
                         "Framework built-ins and `>` resource commands still apply.");
}

#pragma endregion

// ---- The saved feed (Feed_restore / Feed_cacheTick). A snapshot is written to the data partition from a one-shot task:
// flash writes stall the caller for tens to hundreds of ms, which the LVGL task (and the USB screen with it) must not
// sit through. The task needs an internal-RAM stack because it touches flash.
static constexpr const char *kFeedCacheFile = "/feed_cache.json";
static constexpr size_t kFeedCacheMaxBytes = 256 * 1024;
static constexpr int64_t kFeedCacheDebounceMs = 30000;
static const char *TAG_FEEDCACHE = "feedcache";
static std::atomic<bool> g_feedCacheWriting{false};

static void feedCacheWriter(void *arg)
{
    std::string *text = static_cast<std::string *>(arg);
    const bool ok = NightMare::Storage::write(kFeedCacheFile, *text, true);
    ESP_LOGI(TAG_FEEDCACHE, "saved %u bytes: %s", static_cast<unsigned>(text->size()), ok ? "ok" : "FAILED");
    delete text;
    g_feedCacheWriting = false;
    vTaskDelete(nullptr);
}

static void saveFeedCache(const std::string &snapshot)
{
    if (snapshot.size() > kFeedCacheMaxBytes)
    {
        ESP_LOGW(TAG_FEEDCACHE, "snapshot is %u bytes, over the %u limit: not saved", static_cast<unsigned>(snapshot.size()),
                 static_cast<unsigned>(kFeedCacheMaxBytes));
        return;
    }
    if (g_feedCacheWriting.exchange(true))
    {
        return; // the previous save is still running; the next change saves again
    }
    std::string *copy = new std::string(snapshot);
    if (xTaskCreate(feedCacheWriter, "feedsave", 6144, copy, 1, nullptr) != pdPASS)
    {
        delete copy;
        g_feedCacheWriting = false;
    }
}

static bool g_settingsLoaded = false;

// The PC's monitor going off blanks the panel and the LED strip; coming back on wakes them. A touch on the dark panel
// wakes the PC's monitor too. Hibernate and sleep are not handled: the link just drops and the panel keeps its state.
static void wakeBoth()
{
    LedStrip_sleep(false);
    LinkServer_wakePcDisplay();
}

static void followPcDisplay()
{
    static int applied = -1; // the last state acted on: 0 off, 1 on (dimmed changes nothing)
    const int now = LinkServer_pcDisplay();
    if ((now != 0 && now != 1) || now == applied)
    {
        return;
    }
    const int before = applied;
    applied = now;
    if (now == 0)
    {
        LedStrip_sleep(true);
        Rest_screenOffThen(wakeBoth);
    }
    else if (before == 0) // only a real off -> on wakes the panel; a first "on" just says the PC is awake
    {
        LedStrip_sleep(false);
        Rest_wake();
    }
}

static void housekeepingTimer(lv_timer_t *)
{
    static uint32_t appliedRevision = UINT32_MAX;
    static uint32_t lastMirrorMs = 0;

    Mqttp_tick();
    StackportSim_tick(nowMs());
    Ping_tick();
    LedStrip_tick();
    followPcDisplay();
    PcPower_tick();
    SysMon_sample();
    if (!Feed_tick())
    {
        FeedSync_resync(); // a patch for another stream than the cache: start over from version 0
    }
    Feed_cacheTick(esp_timer_get_time() / 1000, kFeedCacheDebounceMs);
    {
        // What the feed costs: one heap line each time its contents changed (the first one is the whole feed arriving).
        static uint32_t loggedRevision = 0;
        if (Feed_revision() != loggedRevision)
        {
            loggedRevision = Feed_revision();
            Heap_log("feed changed");
        }
    }

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
    // Bindings made by name follow the device to its hardware address (its old name is now
    // its label).
    static uint32_t lastFollowMs = 0;
    if (nowMs() - lastFollowMs >= 5000)
    {
        lastFollowMs = nowMs();
        Bindings_followLabels();
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
    Registry_onValue(onValue);
    Net_begin();
    // The Stackport tracker: the bridge's retained status and state sections (App/Stackport.h).
    Net_subscribe(kStackportStatusFilter, true);
    Net_subscribe(kStackportStateFilter, true);

    // 20 ms: fast enough that a tap on a live value feels live, slow enough that a
    // retained-state replay (hundreds of messages) is spread over many frames.
    lv_timer_create(drainTimer, 20, nullptr);
    lv_timer_create(housekeepingTimer, 100, nullptr);

    // A freshly updated image is only kept if it is still running 20 s after boot.
    lv_timer_t *confirm = lv_timer_create([](lv_timer_t *) { Ota_markValid(); }, 20000, nullptr);
    lv_timer_set_repeat_count(confirm, 1);

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
    PcPower_begin();
    Feed_begin(PersistentSettings.get("feed_dismissed", ""),
               [](const std::string &text) { PersistentSettings.set("feed_dismissed", text); });
    // The saved feed: shown at once, and the sync task resumes from its cursor (FeedSync_begin reads it).
    {
        std::string saved;
        if (NightMare::Storage::read(kFeedCacheFile, saved, kFeedCacheMaxBytes) && Feed_restore(saved))
        {
            ESP_LOGI(TAG_FEEDCACHE, "restored %u items (version %lld) from flash", static_cast<unsigned>(Feed_total()),
                     static_cast<long long>(Feed_version()));
        }
    }
    Feed_setCacheSaver(saveFeedCache);
    FeedSync_begin();
    LedStrip_begin();
    LedFeedback_begin();
    LinkServer_begin();
    g_settingsLoaded = true;
    board_lvgl_unlock();
}
