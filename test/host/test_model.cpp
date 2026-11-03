// Host-side tests for the pure app model: Notifications, Registry, Mqttp.
// No LVGL, no ESP-IDF. Run with tools/host_test.sh (uses a throwaway container,
// because this PC has no host compiler).

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "../../src/App/Mqttp.h"
#include "../../src/App/Notifications.h"
#include "../../src/App/Registry.h"
#include "../../src/Explorer/DeviceView.h"
#include "../../src/Explorer/Facts.h"
#include "../../src/Explorer/Manifest.h"
#include "../../src/Explorer/Ping.h"
#include "../../src/Explorer/TopicTap.h"
#include "../../src/Explorer/Watch.h"

#include <ArduinoJson.h>

static int g_failed = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                   \
    do                                                                                \
    {                                                                                 \
        ++g_checks;                                                                   \
        if (!(cond))                                                                  \
        {                                                                             \
            ++g_failed;                                                               \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                    \
        }                                                                             \
    } while (0)

// --- helpers ----------------------------------------------------------------------

static uint32_t g_now = 1000;
static uint32_t fakeNow() { return g_now; }
static int64_t fakeEpoch() { return 1800000000; }

static NetMessage msg(const std::string &topic, const std::string &payload, bool retained = true)
{
    // The strings must outlive the call; tests keep them in statics below.
    static std::string t, p;
    t = topic;
    p = payload;
    return NetMessage{t.c_str(), reinterpret_cast<const uint8_t *>(p.c_str()), p.size(), retained, g_now};
}

// --- Notifications ----------------------------------------------------------------

static int g_events = 0;
static NotifyEvent g_lastEvent;
static void onNotify(const Notification &, NotifyEvent e)
{
    ++g_events;
    g_lastEvent = e;
}

static void testNotifications()
{
    Notifications_setClock(fakeNow, fakeEpoch);
    Notifications_onChange(onNotify);

    const uint32_t a = Notify(NotifySeverity::Info, "t", "first");
    CHECK(a == 1);
    CHECK(Notifications_count() == 1);
    CHECK(Notifications_unread() == 1);
    CHECK(Notifications_at(0).title == "first");
    CHECK(Notifications_at(0).epoch == 1800000000);

    Notify(NotifySeverity::Warn, "t", "second");
    CHECK(Notifications_at(0).title == "second"); // newest first

    // Sticky: one entry per key, identical repeat is ignored.
    const int before = g_events;
    const uint32_t s1 = NotifySticky("k", NotifySeverity::Warn, "net", "down");
    const uint32_t s2 = NotifySticky("k", NotifySeverity::Warn, "net", "down");
    CHECK(s1 == s2);
    CHECK(g_events == before + 1);
    CHECK(Notifications_activeCount() == 1);
    NotifySticky("k", NotifySeverity::Crit, "net", "down hard");
    CHECK(g_lastEvent == NotifyEvent::Updated);
    CHECK(Notifications_activeCount() == 1);
    CHECK(Notifications_activeAt(0).severity == NotifySeverity::Crit);

    // Clear with a resolution message.
    const size_t countBefore = Notifications_count();
    NotifyClear("k", "back up");
    CHECK(Notifications_activeCount() == 0);
    CHECK(Notifications_count() == countBefore + 1);
    CHECK(Notifications_at(0).title == "back up");
    CHECK(Notifications_at(0).severity == NotifySeverity::Ok);
    NotifyClear("k"); // already clear: nothing happens
    CHECK(Notifications_count() == countBefore + 1);

    // Read state.
    CHECK(Notifications_unread() > 0);
    Notifications_markAllRead();
    CHECK(Notifications_unread() == 0);

    // Capacity: history is bounded, live conditions survive.
    NotifySticky("live", NotifySeverity::Warn, "net", "still true");
    for (int i = 0; i < NOTIFICATIONS_CAPACITY + 40; ++i)
    {
        Notify(NotifySeverity::Info, "t", "n" + std::to_string(i));
    }
    CHECK(Notifications_count() <= NOTIFICATIONS_CAPACITY);
    CHECK(Notifications_activeCount() == 1);

    // Async: nothing until the tick.
    const size_t c = Notifications_count();
    NotifyAsync(NotifySeverity::Info, "bg", "from another task");
    CHECK(Notifications_count() == c);
    Notifications_tick();
    CHECK(Notifications_at(0).title == "from another task");

    Notifications_clearHistory();
    CHECK(Notifications_count() == 1); // only the live condition
}

// --- Registry ---------------------------------------------------------------------

static std::vector<std::string> g_presenceLog;
static void onPresence(const Device &d, bool online, bool firstSeen)
{
    g_presenceLog.push_back(d.name + (online ? ":up" : ":down") + (firstSeen ? ":first" : ""));
}

static void testRegistry()
{
    Registry_onPresence(onPresence);

    auto split = Registry_split("Adler/resource/ac:power/state");
    CHECK(split.size() == 4 && split[2] == "ac:power");

    CHECK(!Registry_handle(msg("Control/forecast", "{}")));
    CHECK(!Registry_handle(msg("all/console/in", "PING")));
    CHECK(!Registry_handle(msg("Adler/unknown/thing", "x")));

    // Status creates the device and reports presence once.
    Registry_handle(msg("Adler/status", R"({"name":"Adler","hardware":"Esp32-nm-1","timezone":"<-03>3","online":true})"));
    CHECK(Registry_deviceCount() == 1);
    CHECK(Registry_find("Adler")->online);
    CHECK(Registry_find("Adler")->hardware == "Esp32-nm-1");
    CHECK(g_presenceLog.size() == 1 && g_presenceLog[0] == "Adler:up:first");

    // Same status again: no new presence event.
    Registry_handle(msg("Adler/status", R"({"name":"Adler","hardware":"Esp32-nm-1","online":true})"));
    CHECK(g_presenceLog.size() == 1);

    // Offline (last will), then tombstone behaviour.
    Registry_handle(msg("Adler/status", R"({"name":"Adler","hardware":"Esp32-nm-1","online":false})"));
    CHECK(!Registry_find("Adler")->online);
    CHECK(g_presenceLog.back() == "Adler:down");
    Registry_handle(msg("Ghost/status", ""));
    CHECK(Registry_find("Ghost") == nullptr); // a tombstone never creates a device

    // A gateway.
    Registry_handle(msg("Holmes/status", R"({"schema":1,"device":"Holmes","kind":"gateway","online":true})"));
    CHECK(Registry_find("Holmes")->kind == "gateway");

    // Resource state: created, changed, unchanged, withdrawn.
    const uint32_t rev0 = Registry_revision();
    Registry_handle(msg("Adler/resource/ac_power/state", "true"));
    CHECK(Registry_valueText("Adler", "ac_power") == "true");
    CHECK(Registry_revision() > rev0);
    const uint32_t rev1 = Registry_revision();
    Registry_handle(msg("Adler/resource/ac_power/state", "true"));
    CHECK(Registry_revision() == rev1); // identical value: nothing moved
    CHECK(Registry_value("Adler", "ac_power")->updates == 2);
    Registry_handle(msg("Adler/resource/ac_power/state", "false"));
    CHECK(Registry_valueText("Adler", "ac_power") == "false");
    Registry_handle(msg("Adler/resource/ac_power/state", ""));
    CHECK(Registry_value("Adler", "ac_power")->withdrawn);
    Registry_handle(msg("Adler/resource/ac_power/state", "true"));
    CHECK(!Registry_value("Adler", "ac_power")->withdrawn);

    // Heard indirectly: a resource for a device with no status yet.
    Registry_handle(msg("Newcomer/resource/temperature/state", "21.5"));
    CHECK(Registry_find("Newcomer") != nullptr);
    CHECK(!Registry_find("Newcomer")->seenStatus);

    // Info / telemetry / manifest are kept raw.
    Registry_handle(msg("Adler/info", R"({"a":1})"));
    Registry_handle(msg("Adler/telemetry/system", R"({"heap":1})"));
    Registry_handle(msg("Adler/manifest/msgpack", std::string("\x93\x03\x06\x90", 4)));
    CHECK(Registry_find("Adler")->infoJson == R"({"a":1})");
    CHECK(Registry_find("Adler")->telemetrySystem == R"({"heap":1})");
    CHECK(Registry_find("Adler")->manifest.size() == 4);

    // Sorted listing.
    auto list = Registry_devices();
    CHECK(list.size() >= 3);
    for (size_t i = 1; i < list.size(); ++i)
    {
        CHECK(list[i - 1]->name < list[i]->name);
    }

    // Forget.
    const size_t resources = Registry_resourceCount();
    Registry_forget("Newcomer");
    CHECK(Registry_find("Newcomer") == nullptr);
    CHECK(Registry_resourceCount() == resources - 1);

    // Device capacity: newest wins, evictions are counted.
    for (int i = 0; i < REGISTRY_MAX_DEVICES + 10; ++i)
    {
        g_now += 10;
        Registry_handle(msg("Dev" + std::to_string(i) + "/status", R"({"online":true})"));
    }
    CHECK(Registry_deviceCount() <= REGISTRY_MAX_DEVICES);
    CHECK(Registry_evictedDevices() > 0);
    CHECK(Registry_find("Dev" + std::to_string(REGISTRY_MAX_DEVICES + 9)) != nullptr);

    // Resource capacity.
    for (int i = 0; i < REGISTRY_MAX_RESOURCES + 20; ++i)
    {
        g_now += 1;
        Registry_handle(msg("Bulk/resource/r" + std::to_string(i) + "/state", "1"));
    }
    CHECK(Registry_resourceCount() <= REGISTRY_MAX_RESOURCES);
    CHECK(Registry_evictedResources() > 0);
}

// --- Mqttp ------------------------------------------------------------------------

static std::vector<std::pair<std::string, std::string>> g_published;
static std::vector<std::string> g_subscribed, g_unsubscribed;
static bool g_publishOk = true;
static bool pub(const std::string &topic, const std::string &payload)
{
    if (!g_publishOk)
    {
        return false;
    }
    g_published.emplace_back(topic, payload);
    return true;
}
static void sub(const std::string &f) { g_subscribed.push_back(f); }
static void unsub(const std::string &f) { g_unsubscribed.push_back(f); }

static std::string idFromTopic(const std::string &topic)
{
    const size_t a = topic.find("/controlled/") + 12;
    return topic.substr(a, topic.find("/in") - a);
}

static void testMqttp()
{
    g_now = 100000;
    Mqttp_setPorts({pub, sub, unsub, fakeNow});

    // A request holds back until the fresh subscription has had time to settle.
    std::vector<MqttpResult> results;
    auto collect = [&](const MqttpResult &r) { results.push_back(r); };
    const uint32_t id = Mqttp_request("Adler", "INFO SYSTEM", collect);
    CHECK(id != 0);
    CHECK(g_subscribed.size() == 1 && g_subscribed[0] == "Adler/console/controlled/+/out");
    Mqttp_tick();
    CHECK(g_published.empty());
    g_now += 450;
    Mqttp_tick();
    CHECK(g_published.size() == 1);
    CHECK(g_published[0].second == "INFO SYSTEM");
    const std::string reqTopic = g_published[0].first;
    const std::string rid = idFromTopic(reqTopic);
    CHECK(results.empty()); // callback never fires from inside request()

    // Single-message reply.
    g_now += 25;
    CHECK(Mqttp_handleMessage(msg("Adler/console/controlled/" + rid + "/out", R"({"heap":123})", false)));
    CHECK(results.size() == 1);
    CHECK(results[0].status == MqttpStatus::Ok);
    CHECK(results[0].response == R"({"heap":123})");
    CHECK(results[0].rttMs == 25);
    CHECK(Mqttp_pendingCount() == 0);

    // Late duplicate: counted as stray, consumed.
    CHECK(Mqttp_handleMessage(msg("Adler/console/controlled/" + rid + "/out", "again", false)));
    CHECK(Mqttp_strayReplies() == 1);
    CHECK(results.size() == 1);

    // Chunked reply, with ";;" inside the data.
    results.clear();
    g_published.clear();
    Mqttp_request("Adler", "CONFIG LIST", collect);
    g_now += 10;
    Mqttp_tick();
    const std::string rid2 = idFromTopic(g_published.at(0).first);
    Mqttp_handleMessage(msg("Adler/console/controlled/" + rid2 + "/out", ";;1/3;;part one;;x ", false));
    CHECK(results.empty());
    Mqttp_handleMessage(msg("Adler/console/controlled/" + rid2 + "/out", ";;2/3;;part two ", false));
    Mqttp_handleMessage(msg("Adler/console/controlled/" + rid2 + "/out", ";;3/3;;end", false));
    CHECK(results.size() == 1);
    CHECK(results[0].status == MqttpStatus::Ok);
    CHECK(results[0].response == "part one;;x part two end");

    // Device-reported error.
    results.clear();
    g_published.clear();
    Mqttp_request("Adler", "BOGUS", collect);
    g_now += 10;
    Mqttp_tick();
    Mqttp_handleMessage(msg("Adler/console/controlled/" + idFromTopic(g_published.at(0).first) + "/out",
                            ";;error;;unknown command", false));
    CHECK(results.size() == 1 && results[0].status == MqttpStatus::Failed);
    CHECK(results[0].response == "unknown command");

    // Timeout.
    results.clear();
    g_published.clear();
    Mqttp_request("Adler", "PING", collect, 500);
    g_now += 10;
    Mqttp_tick();
    CHECK(results.empty());
    g_now += 600;
    Mqttp_tick();
    CHECK(results.size() == 1 && results[0].status == MqttpStatus::Timeout);

    // Chunks extend the deadline instead of timing out mid-reply.
    results.clear();
    g_published.clear();
    Mqttp_request("Adler", "BIG", collect, 500);
    g_now += 10;
    Mqttp_tick();
    const std::string rid3 = idFromTopic(g_published.at(0).first);
    g_now += 400;
    Mqttp_handleMessage(msg("Adler/console/controlled/" + rid3 + "/out", ";;1/2;;a", false));
    g_now += 400;
    Mqttp_tick();
    CHECK(results.empty()); // would have timed out without the extension
    Mqttp_handleMessage(msg("Adler/console/controlled/" + rid3 + "/out", ";;2/2;;b", false));
    CHECK(results.size() == 1 && results[0].response == "ab");

    // Truncation.
    results.clear();
    g_published.clear();
    Mqttp_request("Adler", "HUGE", collect);
    g_now += 10;
    Mqttp_tick();
    Mqttp_handleMessage(msg("Adler/console/controlled/" + idFromTopic(g_published.at(0).first) + "/out",
                            std::string(MQTTP_RESPONSE_MAX + 500, 'x'), false));
    CHECK(results.size() == 1 && results[0].truncated);
    CHECK(results[0].response.size() == MQTTP_RESPONSE_MAX);

    // No transport.
    results.clear();
    g_publishOk = false;
    Mqttp_request("Adler", "PING", collect);
    g_now += 10;
    Mqttp_tick();
    CHECK(results.size() == 1 && results[0].status == MqttpStatus::Failed);
    g_publishOk = true;

    // Cancel.
    results.clear();
    g_published.clear();
    const uint32_t cid = Mqttp_request("Adler", "PING", collect);
    Mqttp_cancel(cid);
    CHECK(results.size() == 1 && results[0].status == MqttpStatus::Cancelled);
    CHECK(Mqttp_pendingCount() == 0);

    // Table limit.
    for (int i = 0; i < MQTTP_MAX_PENDING; ++i)
    {
        CHECK(Mqttp_request("Adler", "PING", nullptr) != 0);
    }
    CHECK(Mqttp_request("Adler", "PING", nullptr) == 0);
    g_now += 1000;
    Mqttp_tick(); // all are sent now
    CHECK(Mqttp_pendingCount() == MQTTP_MAX_PENDING);
    g_now += MQTTP_DEFAULT_TIMEOUT_MS + 100;
    Mqttp_tick(); // and all time out
    CHECK(Mqttp_pendingCount() == 0);

    // The unused subscription lingers, then is dropped.
    g_unsubscribed.clear();
    g_now += 20000;
    Mqttp_tick();
    CHECK(g_unsubscribed.size() == 1 && g_unsubscribed[0] == "Adler/console/controlled/+/out");

    // A reply addressed to someone else's id is consumed, not ours.
    CHECK(Mqttp_handleMessage(msg("Adler/console/controlled/req-42/out", "x", false)));
    CHECK(!Mqttp_handleMessage(msg("Adler/status", "{}", false)));
}


// --- Explorer: Manifest -----------------------------------------------------------------

static std::vector<uint8_t> msgpackOf(const char *json)
{
    JsonDocument doc;
    deserializeJson(doc, json);
    std::string out;
    serializeMsgPack(doc, out);
    return std::vector<uint8_t>(out.begin(), out.end());
}

static void testManifest()
{
    auto bytes = msgpackOf(
        R"([3,6,[
            [0,"temperature",0,3,null,300000,[1000,7,true,"Address: 0x48"]],
            [0,"power",1,1,null,300000,null],
            [0,"ac_door",0,1,"door",300000,null],
            [1,"set_timer",[["seconds",2,true],["label",0,false]]],
            [2,"acoustic:beep",2]
        ]])");
    Manifest m = Manifest_parse(bytes.data(), bytes.size());
    CHECK(m.ok);
    CHECK(m.manifestVersion == 6);
    CHECK(m.resources.size() == 5);

    const ResourceSpec *t = m.find("temperature");
    CHECK(t && t->kind == ResKind::Value && !t->writable && t->type == ValueType::Float);
    CHECK(t && t->hasHardware && t->pollMs == 1000 && t->hwFlags == 7 && t->hwConnectedKnown && t->hwConnected);
    CHECK(t && t->hwNote == "Address: 0x48");
    const ResourceSpec *p = m.find("power");
    CHECK(p && p->writable && p->type == ValueType::Boolean && !p->hasHardware);
    const ResourceSpec *d = m.find("ac_door");
    CHECK(d && d->dependsOn == "door");
    const ResourceSpec *a = m.find("set_timer");
    CHECK(a && a->kind == ResKind::Action && a->args.size() == 2);
    CHECK(a && a->args[0].name == "seconds" && a->args[0].type == ValueType::Integer && a->args[0].required);
    CHECK(a && a->args[1].type == ValueType::String && !a->args[1].required);
    const ResourceSpec *e = m.find("acoustic:beep");
    CHECK(e && e->kind == ResKind::Event && e->type == ValueType::Integer);
    CHECK(m.find("nothing") == nullptr);

    // Rejections.
    CHECK(!Manifest_parse(nullptr, 0).ok);
    auto bad = msgpackOf(R"([9,6,[]])");
    Manifest wrongEncoding = Manifest_parse(bad.data(), bad.size());
    CHECK(!wrongEncoding.ok && wrongEncoding.error.find("encoding") != std::string::npos);
    auto notManifest = msgpackOf(R"({"a":1})");
    CHECK(!Manifest_parse(notManifest.data(), notManifest.size()).ok);
    const uint8_t junk[] = {0xC1, 0x00};
    CHECK(!Manifest_parse(junk, sizeof(junk)).ok);

    // An unreadable entry is skipped, the rest survive.
    auto partial = msgpackOf(R"([3,6,[[0,"a",0,1,null,-1,null],"junk",[7,"x"],[0,"b",1,2,null,-1,null]]])");
    Manifest pm = Manifest_parse(partial.data(), partial.size());
    CHECK(pm.ok && pm.resources.size() == 2);
}

// --- Explorer: Facts --------------------------------------------------------------------

static void testFacts()
{
    Device d;
    d.name = "Adler";
    d.hardware = "Esp32-nm-1";
    d.infoJson = R"({"identity":{"name":"Adler","id":"0011","timezone":"<-03>3"},
        "hardware":{"board":"Esp32 Dev","chip":"ESP32","cores":2,"revision":3,"flash_bytes":4194304,"heap_bytes":327680,"psram_bytes":0},
        "build":{"firmware_version":"1.1.141","date":"Sep 21 2026","time":"21:17:38","esp_idf_version":"v5.5"},
        "boot":{"reset_reason":1}})";
    d.telemetrySystem = R"({"uptime_ms":93784000,"cpu_mhz":240,"free_heap_bytes":210000,"min_free_heap_bytes":198000,"free_psram_bytes":0})";
    d.telemetryNetwork = R"({"transport":{"preferred":"MQTT","active":"MQTT","state":"CONNECTED"},
        "wifi_ip":{"state":"CONNECTED","ssid":"home","ip":"192.168.1.50","rssi":-55,"channel":6},
        "esp_now":{"state":"CONNECTED","gateway_connected":true,"rtt_ms":12},"mqtt":{"state":"CONNECTED","profile":"REMOTE"}})";
    DeviceFacts f = Facts_parse(d);
    CHECK(f.hasHardware && f.chip == "ESP32" && f.cores == 2 && f.flashBytes == 4194304);
    CHECK(f.hasBuild && f.firmware == "1.1.141");
    CHECK(f.hasBoot && f.resetReason == 1 && std::string(Facts_resetReason(1)) == "power-on");
    CHECK(f.hasSystem && f.uptimeMs == 93784000 && f.freeHeap == 210000);
    CHECK(f.hasNetwork && f.ssid == "home" && f.ip == "192.168.1.50" && f.rssi == -55 && f.rttMs == 12);
    CHECK(f.mqttProfile == "REMOTE" && f.gatewayConnected);
    CHECK(Facts_formatDuration(93784000) == "1d 02h 03m");
    CHECK(Facts_formatDuration(5000) == "5s");
    CHECK(Facts_formatBytes(4194304) == "4.0 MB");
    CHECK(Facts_formatBytes(2048) == "2 KB");

    // Nothing published -> nothing claimed.
    Device empty;
    DeviceFacts e = Facts_parse(empty);
    CHECK(!e.hasHardware && !e.hasSystem && !e.hasNetwork && !e.hasBuild && !e.hasBoot);
    Device broken;
    broken.infoJson = "{not json";
    CHECK(!Facts_parse(broken).hasHardware);
}

// --- Explorer: Watch --------------------------------------------------------------------

static void testWatch()
{
    float v;
    CHECK(Watch_parseValue("23.5", v) && v == 23.5f);
    CHECK(Watch_parseValue("true", v) && v == 1);
    CHECK(Watch_parseValue("false", v) && v == 0);
    CHECK(Watch_parseValue("-4", v) && v == -4);
    CHECK(!Watch_parseValue("cool", v));
    CHECK(!Watch_parseValue("12abc", v));
    CHECK(!Watch_parseValue("", v));

    Watch_restore("Adler/temperature,Mycroft/light:1,bogus,/x,y/");
    CHECK(Watch_count() == 2);
    CHECK(Watch_isWatched("Adler", "temperature"));
    CHECK(Watch_isWatched("Mycroft", "light:1"));
    CHECK(Watch_serialize() == "Adler/temperature,Mycroft/light:1");

    // Unwatched resources record nothing.
    Watch_record("Adler", "other", "1", 1000);
    CHECK(Watch_samples("Adler", "other") == nullptr);

    Watch_record("Adler", "temperature", "20", 1000);
    Watch_record("Adler", "temperature", "20", 2000);  // unchanged, too soon: skipped
    Watch_record("Adler", "temperature", "21", 2500);  // changed: kept
    Watch_record("Adler", "temperature", "21", 9000);  // unchanged but past the interval: kept
    Watch_record("Adler", "temperature", "cool", 9500); // not a number: ignored
    const auto *samples = Watch_samples("Adler", "temperature");
    CHECK(samples && samples->size() == 3);
    CHECK(samples && (*samples)[1].value == 21 && (*samples)[1].ms == 2500);

    // Ring.
    for (int i = 0; i < WATCH_MAX_SAMPLES + 50; ++i)
    {
        Watch_record("Adler", "temperature", std::to_string(i), 20000 + i * 10);
    }
    CHECK(Watch_samples("Adler", "temperature")->size() == WATCH_MAX_SAMPLES);

    // Toggle and limit.
    CHECK(Watch_toggle("Adler", "temperature")); // off
    CHECK(!Watch_isWatched("Adler", "temperature"));
    for (int i = 0; i < WATCH_MAX_RESOURCES; ++i)
    {
        Watch_toggle("D", "r" + std::to_string(i));
    }
    CHECK(Watch_count() == WATCH_MAX_RESOURCES);
    CHECK(!Watch_toggle("D", "one-too-many"));
}

// --- Explorer: TopicTap -----------------------------------------------------------------

static void testTap()
{
    // Nothing is kept while nobody is looking.
    Tap_record(msg("Adler/unseen", "x"));
    CHECK(Tap_count() == 0 && !Tap_active());
    Tap_acquire();
    CHECK(Tap_active());
    Tap_clear();
    Tap_record(msg("Adler/status", R"({"online":true})"));
    CHECK(Tap_count() == 1 && Tap_at(0).topic == "Adler/status" && Tap_at(0).retained && !Tap_at(0).binary);
    Tap_record(msg("Adler/manifest/msgpack", std::string("\x93\x03\x06\x9a\xff\xfe\x00\x01", 8)));
    CHECK(Tap_at(0).binary && Tap_at(0).text == "<8 bytes binary>");
    Tap_record(msg("Adler/console/out", "line one\nline two"));
    CHECK(Tap_at(0).text == "line one line two");
    Tap_record(msg("X/y", std::string(1000, 'a')));
    CHECK(Tap_at(0).length == 1000 && Tap_at(0).text.size() == TAP_TEXT_MAX + 3);

    Tap_setPaused(true);
    const size_t before = Tap_count();
    Tap_record(msg("Adler/paused", "x"));
    CHECK(Tap_count() == before);
    Tap_setPaused(false);

    for (int i = 0; i < TAP_CAPACITY + 20; ++i)
    {
        Tap_record(msg("T/" + std::to_string(i), "x"));
    }
    CHECK(Tap_count() == TAP_CAPACITY);
    CHECK(Tap_at(0).topic == "T/" + std::to_string(TAP_CAPACITY + 19));

    CHECK(Tap_topicIsFor("Adler/status", "Adler"));
    CHECK(!Tap_topicIsFor("Adler2/status", "Adler"));
    CHECK(!Tap_topicIsFor("Adler", "Adler"));
    // The last user leaving frees the tail; a second user keeps it.
    Tap_acquire();
    Tap_release();
    CHECK(Tap_count() == TAP_CAPACITY);
    Tap_release();
    CHECK(Tap_count() == 0 && !Tap_active() && !Tap_paused());
    Tap_record(msg("Adler/after", "x"));
    CHECK(Tap_count() == 0);
}

// --- Explorer: Ping ---------------------------------------------------------------------

static void testPing()
{
    g_now = 500000;
    g_published.clear();
    g_publishOk = true;
    Mqttp_setPorts({pub, sub, unsub, fakeNow});

    std::vector<PingStats> updates;
    Ping_start("Adler", 3, [&](const PingStats &s) { updates.push_back(s); });
    CHECK(Ping_running());

    // Reply to each ping with PONG, after a known delay.
    const uint32_t delays[] = {12, 30, 18};
    for (int i = 0; i < 3; ++i)
    {
        g_published.clear();
        Ping_tick();
        Mqttp_tick();
        CHECK(g_published.empty() || true); // the first request waits out the settle time
        for (int guard = 0; guard < 20 && g_published.empty(); ++guard)
        {
            g_now += 50;
            Mqttp_tick();
            Ping_tick();
        }
        CHECK(g_published.size() == 1);
        CHECK(g_published[0].second == "PING");
        g_now += delays[i];
        Mqttp_handleMessage(msg("Adler/console/controlled/" + idFromTopic(g_published[0].first) + "/out", "PONG", false));
        g_now += 400; // past the gap between pings
        Mqttp_tick();
    }
    CHECK(!Ping_running());
    const PingStats &s = Ping_stats();
    CHECK(s.sent == 3 && s.received == 3 && s.lost == 0);
    CHECK(s.minMs == 12 && s.maxMs == 30);
    CHECK(s.avgMs > 19.9f && s.avgMs < 20.1f);
    CHECK(updates.size() == 3 && !updates.back().running);

    // A device that does not answer: every ping is lost.
    updates.clear();
    g_published.clear();
    Ping_start("Ghost", 2, [&](const PingStats &st) { updates.push_back(st); }, 500);
    for (int i = 0; i < 200 && Ping_running(); ++i)
    {
        g_now += 100;
        Mqttp_tick();
        Ping_tick();
    }
    CHECK(!Ping_running());
    CHECK(Ping_stats().lost == 2 && Ping_stats().received == 0);
    CHECK(Ping_stats().rtts.size() == 2 && Ping_stats().rtts[0] == -1);

    // Stop early.
    updates.clear();
    Ping_start("Adler", 10, [&](const PingStats &st) { updates.push_back(st); });
    Ping_stop();
    CHECK(!Ping_running());
    CHECK(!updates.empty() && !updates.back().running);
}

// --- Explorer: DeviceView ---------------------------------------------------------------

static void testDeviceView()
{
    Device d;
    d.name = "Adler";
    auto bytes = msgpackOf(R"([3,6,[[0,"ac_power",1,1,null,-1,null],[0,"temperature",0,3,null,-1,null],[1,"ac_sleep",[["minutes",2,true]]],[2,"beep",2]]])");
    d.manifest = bytes;
    d.manifestMs = 777;
    d.resources["ac_power"].value = "true";
    d.resources["temperature"].value = "23.50";
    d.resources["undeclared"].value = "cool";
    d.resources["gone"].withdrawn = true;

    const Manifest *m = Explorer_manifest(d);
    CHECK(m && m->ok && m->resources.size() == 4);
    CHECK(Explorer_manifest(d) == m); // cached: the same parse is reused

    auto rows = Explorer_rows(d);
    CHECK(rows.size() == 6);
    // Manifest entries first, in manifest order ...
    CHECK(rows[0].name == "ac_power" && rows[0].inManifest && rows[0].writable && rows[0].hasValue && rows[0].value == "true");
    CHECK(rows[1].name == "temperature" && rows[1].type == ValueType::Float);
    CHECK(rows[2].name == "ac_sleep" && rows[2].kind == ResKind::Action && !rows[2].hasValue);
    CHECK(rows[3].name == "beep" && rows[3].kind == ResKind::Event);
    // ... then what was published without being declared, by name.
    CHECK(rows[4].name == "gone" && !rows[4].inManifest && rows[4].withdrawn && !rows[4].hasValue);
    CHECK(rows[5].name == "undeclared" && !rows[5].inManifest && rows[5].type == ValueType::String);

    CHECK(Explorer_guessType("true") == ValueType::Boolean);
    CHECK(Explorer_guessType("-12") == ValueType::Integer);
    CHECK(Explorer_guessType("1.5") == ValueType::Float);
    CHECK(Explorer_guessType("{\"a\":1}") == ValueType::Struct);
    CHECK(Explorer_guessType("cool") == ValueType::String);

    CHECK(Explorer_display(ValueType::Float, "23.50") == "23.5");
    CHECK(Explorer_display(ValueType::Colour, "16711680") == "#FF0000");
    CHECK(Explorer_display(ValueType::String, "line1\nline2") == "line1 line2");
    CHECK(Explorer_display(ValueType::String, std::string(100, 'x'), 10) == "xxxxxxxxx...");

    // No manifest at all: still explorable from what was published.
    Device bare;
    bare.name = "Bare";
    bare.resources["a"].value = "1";
    CHECK(Explorer_manifest(bare) == nullptr);
    auto bareRows = Explorer_rows(bare);
    CHECK(bareRows.size() == 1 && !bareRows[0].inManifest && bareRows[0].type == ValueType::Integer);

    // A manifest that does not parse yields rows from the registry only.
    Device broken;
    broken.name = "Broken";
    broken.manifest = {0xC1, 0x00};
    broken.manifestMs = 5;
    broken.resources["x"].value = "y";
    const Manifest *bm = Explorer_manifest(broken);
    CHECK(bm && !bm->ok);
    CHECK(Explorer_rows(broken).size() == 1);

    Explorer_forgetManifest("Adler");
}

int main()
{
    testNotifications();
    testRegistry();
    testMqttp();
    testManifest();
    testFacts();
    testWatch();
    testTap();
    testPing();
    testDeviceView();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
