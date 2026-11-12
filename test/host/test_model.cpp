// Host-side tests for the pure app model: Notifications, Registry, Mqttp.
// No LVGL, no ESP-IDF. Run with tools/host_test.sh (uses a throwaway container,
// because this PC has no host compiler).

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "../../src/App/Battery.h"
#include "../../src/App/Calendar.h"
#include "../../src/Link/LinkProto.h"
#include "../../src/App/SysMon.h"
#include "../../src/App/Feed.h"
#include "../../src/App/Mqttp.h"
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

// --- Feed -------------------------------------------------------------------------

static std::string g_feedSaved;
static void feedSave(const std::string &text) { g_feedSaved = text; }

static FeedDelta parseOk(const char *json)
{
    FeedDelta d;
    CHECK(Feed_parse(json, strlen(json), d));
    return d;
}

static void testFeed()
{
    // The spec's examples.
    FeedDelta initial = parseOk(R"({"version":1,"epoch":"e1","full":true,"items":[
        {"numeric_id":1,"id":"linear:MW-42","source":"linear","kind":"task","title":"Review","status":"todo"},
        {"numeric_id":7,"id":"linear:MW-9","source":"linear","kind":"task","title":"Old","status":"todo"}],
        "removed_ids":[],"sources":{},"connections":[]})");
    CHECK(initial.version == 1 && initial.epoch == "e1" && initial.full && initial.items.size() == 2);
    CHECK(initial.items[0].numericId == 1 && initial.items[0].title == "Review");

    Feed_clear();
    Feed_begin("", feedSave);
    CHECK(Feed_apply(initial));
    CHECK(Feed_version() == 1 && Feed_epoch() == "e1" && Feed_visibleCount() == 2);
    CHECK(Feed_visibleAt(0).numericId == 1 && Feed_visibleAt(1).numericId == 7);

    // Dismiss is local, persisted with the epoch, and survives an update of the same item.
    const uint32_t rev = Feed_revision();
    Feed_dismiss(1);
    CHECK(Feed_revision() != rev && Feed_visibleCount() == 1 && Feed_visibleAt(0).numericId == 7);
    CHECK(g_feedSaved == "e1;1");
    CHECK(Feed_apply(parseOk(R"({"version":2,"epoch":"e1","full":false,
        "items":[{"numeric_id":1,"id":"linear:MW-42","source":"linear","kind":"task","title":"Review - updated","status":"in_progress"}],
        "removed_ids":[]})")));
    CHECK(Feed_total() == 2 && Feed_visibleCount() == 1);

    // Patch: update + remove. Removing a dismissed item drops its dismissal.
    CHECK(Feed_apply(parseOk(R"({"version":3,"epoch":"e1","full":false,"items":[],"removed_ids":[1,7,99]})")));
    CHECK(Feed_total() == 0 && Feed_version() == 3 && g_feedSaved == "e1;");

    // Unchanged poll: same version, nothing to do, no revision bump.
    const uint32_t quiet = Feed_revision();
    CHECK(Feed_apply(parseOk(R"({"version":3,"epoch":"e1","full":false,"items":[],"removed_ids":[]})")));
    CHECK(Feed_revision() == quiet);

    // A retried delta is idempotent.
    FeedDelta again = parseOk(R"({"version":4,"epoch":"e1","full":false,
        "items":[{"numeric_id":5,"title":"New"}],"removed_ids":[]})");
    CHECK(Feed_apply(again) && Feed_apply(again) && Feed_total() == 1);

    // A patch for another epoch is refused and leaves the cache alone.
    CHECK(!Feed_apply(parseOk(R"({"version":9,"epoch":"other","full":false,"items":[],"removed_ids":[5]})")));
    CHECK(Feed_total() == 1 && Feed_version() == 4);

    // A full sync of a new epoch forgets the old dismissals (ids mean something else now).
    Feed_dismiss(5);
    CHECK(Feed_visibleCount() == 0);
    CHECK(Feed_apply(parseOk(R"({"version":1,"epoch":"e2","full":true,
        "items":[{"numeric_id":5,"title":"Different item, same number"}],"removed_ids":[]})")));
    CHECK(Feed_visibleCount() == 1 && Feed_epoch() == "e2" && g_feedSaved == "e2;");

    // Dismissals loaded from the store apply when the same epoch syncs again, and not otherwise.
    Feed_clear();
    Feed_begin("e2;5", feedSave);
    CHECK(Feed_apply(parseOk(R"({"version":1,"epoch":"e2","full":true,"items":[{"numeric_id":5,"title":"x"},{"numeric_id":6,"title":"y"}]})")));
    CHECK(Feed_visibleCount() == 1 && Feed_visibleAt(0).numericId == 6);
    Feed_clear();
    Feed_begin("e1;5", feedSave);
    CHECK(Feed_apply(parseOk(R"({"version":1,"epoch":"e2","full":true,"items":[{"numeric_id":5,"title":"x"}]})")));
    CHECK(Feed_visibleCount() == 1);

    // Thread-safe hand-over.
    Feed_post(parseOk(R"({"version":2,"epoch":"e2","full":false,"items":[{"numeric_id":8,"title":"z"}]})"));
    CHECK(Feed_total() == 1);
    CHECK(Feed_tick() && Feed_total() == 2);
    Feed_post(parseOk(R"({"version":3,"epoch":"zzz","full":false})"));
    CHECK(!Feed_tick());

    Feed_dismissAll();
    CHECK(Feed_visibleCount() == 0);

    // Malformed answers are rejected whole.
    FeedDelta d;
    CHECK(!Feed_parse("", 0, d));
    CHECK(!Feed_parse("[]", 2, d));
    CHECK(!Feed_parse(R"({"items":[]})", 12, d));
    CHECK(!Feed_parse(R"({"version":1.5})", 15, d));
    CHECK(!Feed_parse(R"({"version":"1"})", 15, d));
    CHECK(!Feed_parse(R"({"version":-1})", 14, d));
    CHECK(!Feed_parse(R"({"version":1,"items":[{"title":"no id"}]})", 41, d));
    CHECK(!Feed_parse(R"({"version":1,"items":[{"numeric_id":0}]})", 39, d));
    CHECK(!Feed_parse(R"({"version":1,"items":{}})", 25, d));
    CHECK(!Feed_parse(R"({"version":1,"removed_ids":["7"]})", 35, d));
}

// --- Calendar ---------------------------------------------------------------------

static void testCalendar()
{
    setenv("TZ", "<-03>3", 1); // the panel lives at UTC-3
    tzset();

    int64_t t = 0;
    bool dateOnly = true;
    CHECK(Cal_parseTime("2026-11-10T14:00:00-03:00", t, dateOnly) && !dateOnly);
    CHECK(t == 1794330000); // 2026-11-10 17:00:00Z
    int64_t z = 0;
    CHECK(Cal_parseTime("2026-11-10T17:00:00Z", z, dateOnly) && z == t);
    CHECK(Cal_parseTime("2026-11-10T17:00:00.250+00:00", z, dateOnly) && z == t);
    CHECK(Cal_parseTime("2026-11-10", z, dateOnly) && dateOnly && z == 1794279600); // local midnight = 03:00Z
    CHECK(!Cal_parseTime("", z, dateOnly));
    CHECK(!Cal_parseTime("2026-13-01", z, dateOnly));
    CHECK(!Cal_parseTime("2026-11-10T14:00:00", z, dateOnly)); // no offset: ambiguous
    CHECK(!Cal_parseTime("tomorrow", z, dateOnly));

    const int64_t day = Cal_dayStart(t); // 2026-11-10 local midnight
    CHECK(day == 1794279600);
    CHECK(Cal_dayStart(t, 1) == day + 86400 && Cal_dayStart(t, -1) == day - 86400);

    Feed_clear();
    Feed_begin("", feedSave);
    FeedDelta d = parseOk(R"({"version":1,"epoch":"c1","full":true,"items":[
      {"numeric_id":1,"id":"g:1","source":"google_calendar","kind":"event","title":"Standup",
       "starts_at":"2026-11-10T14:00:00-03:00","ends_at":"2026-11-10T14:30:00-03:00","connection":{"alias":"Work"}},
      {"numeric_id":2,"id":"g:2","source":"google_calendar","kind":"event","title":"Holiday",
       "starts_at":"2026-11-10","ends_at":"2026-11-12"},
      {"numeric_id":3,"id":"g:3","source":"google_calendar","kind":"event","title":"Late call",
       "starts_at":"2026-11-10T23:30:00-03:00","ends_at":"2026-11-11T00:30:00-03:00"},
      {"numeric_id":4,"id":"w:4","source":"woi","kind":"task","title":"Ship it","due_at":"2026-11-11"},
      {"numeric_id":5,"id":"l:5","source":"linear","kind":"task","title":"Someday","status":"todo"},
      {"numeric_id":6,"id":"s:6","source":"stackport","kind":"service","title":"Down","status":"down"},
      {"numeric_id":8,"id":"s:8","source":"stackport","kind":"service","title":"Fine","status":"up"},
      {"numeric_id":9,"id":"l:9","source":"linear","kind":"task","title":"Doing","status":"in_progress","due_at":"2026-11-20"},
      {"numeric_id":10,"id":"l:10","source":"linear","kind":"task","title":"Soon","status":"todo","due_at":"2026-11-12"},
      {"numeric_id":11,"id":"w:11","source":"woi","kind":"task","title":"No date","status":"todo"},
      {"numeric_id":7,"id":"g:7","source":"google_calendar","kind":"event","title":"Broken","starts_at":"garbage"}]})");
    CHECK(Feed_apply(d));

    CHECK(Cal_count() == 4); // 1,2,3 and the due task; the broken event and the undated/service items are not entries
    const auto today = Cal_onDay(day);
    CHECK(today.size() == 3 && today[0]->title == "Holiday" && today[1]->title == "Standup" && today[2]->title == "Late call");
    CHECK(today[0]->allDay && today[1]->account == "Work");
    const auto tomorrow = Cal_onDay(Cal_dayStart(t, 1));
    CHECK(tomorrow.size() == 3); // the holiday still covers it, the late call runs over midnight, the task is due
    CHECK(Cal_onDay(Cal_dayStart(t, 2)).empty()); // the holiday's end is exclusive: it does not touch the 12th

    {
        // Todos (Linear + WOI): in progress first, then by due date, undated last.
        const auto todos = Cal_todos();
        CHECK(todos.size() == 5 && todos[0]->title == "Doing" && todos[1]->title == "Ship it" &&
              todos[2]->title == "Soon");
    }
    CHECK(Cal_tasks("", -1).size() == 5);
    CHECK(Cal_tasks("linear", -1).size() == 3 && Cal_tasks("woi", -1).size() == 2);
    CHECK(Cal_tasks("", static_cast<int>(TaskGroup::Doing)).size() == 1);
    CHECK(Cal_tasks("linear", static_cast<int>(TaskGroup::Todo)).size() == 2); // "Someday", "Soon"
    CHECK(Cal_tasks("", static_cast<int>(TaskGroup::Review)).empty());
    CHECK(std::string(Cal_taskGroupName(TaskGroup::Review)) == "Review");
    {
        FeedItem review;
        review.status = "In Review";
        CHECK(Cal_taskGroup(review) == TaskGroup::Review);
        review.status = "backlog";
        CHECK(Cal_taskGroup(review) == TaskGroup::Todo);
        review.status = "started";
        CHECK(Cal_taskGroup(review) == TaskGroup::Doing);
    }
    CHECK(Cal_alerts().size() == 1 && Cal_alerts()[0]->title == "Down"); // "up" services are not alerts
    CHECK(Cal_services().total == 2 && Cal_services().down == 1);

    {
        const auto attention = Cal_attention();
        CHECK(attention.size() == 6 && attention[0]->title == "Down" && attention[1]->title == "Doing");
    }

    // Standup is 17:00Z..17:30Z.
    CHECK(Cal_happening(t - 60) == nullptr);
    CHECK(Cal_happening(t + 60) && Cal_happening(t + 60)->title == "Standup");
    CHECK(Cal_happening(t + 1800) == nullptr);
    CHECK(Cal_next(t - 3600) && Cal_next(t - 3600)->title == "Standup");
    CHECK(Cal_next(t + 60) && Cal_next(t + 60)->title == "Late call");
    CHECK(Cal_next(t + 86400) == nullptr);

    // A dismissed task drops off the calendar; an event cannot be dismissed away from it.
    Feed_dismiss(4);
    CHECK(Cal_onDay(Cal_dayStart(t, 1)).size() == 2);
    CHECK(Cal_todos().size() == 4); // the dismissed WOI deadline is gone from the todos too
    Feed_dismiss(5);
    CHECK(Cal_todos().size() == 3);
}

void testFeedCache()
{
    Feed_clear();
    Feed_begin("", feedSave);
    FeedDelta d = parseOk(R"({"version":5,"epoch":"e1","full":true,"items":[
      {"numeric_id":1,"id":"l:1","source":"linear","kind":"task","title":"Pão à noite","status":"todo","due_at":"2026-11-12","connection":{"alias":"Work"}},
      {"numeric_id":2,"id":"g:2","source":"google_calendar","kind":"event","title":"Standup","starts_at":"2026-11-12T14:00:00-03:00","ends_at":"2026-11-12T14:30:00-03:00","url":"https://calendar.google.com/calendar/event?eid=x","join_url":"https://meet.google.com/abc-defg-hij"}]})");
    CHECK(Feed_apply(d));

    // snapshot -> empty cache -> restore: same content, same cursor
    const std::string snapshot = Feed_serialize();
    Feed_clear();
    CHECK(Feed_total() == 0);
    CHECK(Feed_restore(snapshot));
    CHECK(Feed_total() == 2 && Feed_version() == 5 && Feed_epoch() == "e1");
    CHECK(Feed_find(1) && Feed_find(1)->title == "P\xC3\xA3o \xC3\xA0 noite" && Feed_find(1)->account == "Work" &&
          Feed_find(1)->dueAt == "2026-11-12" && Feed_find(1)->status == "todo");
    CHECK(Feed_find(2) && Feed_find(2)->startsAt == "2026-11-12T14:00:00-03:00");
    CHECK(Feed_find(2)->joinUrl == "https://meet.google.com/abc-defg-hij" && Feed_find(2)->url.rfind("https://calendar.google.com", 0) == 0);
    CHECK(Feed_find(1)->joinUrl.empty()); // a task has none
    CHECK(!Feed_restore("garbage") && Feed_total() == 2); // a bad file leaves the cache alone

    // the saver: one write per burst of changes, none for what was just restored
    static int saves;
    static std::string last;
    saves = 0;
    Feed_setCacheSaver([](const std::string &text) {
        ++saves;
        last = text;
    });
    CHECK(!Feed_cacheTick(1000, 30000)); // restore left it clean
    FeedDelta patch = parseOk(R"({"version":6,"epoch":"e1","items":[{"numeric_id":3,"id":"l:3","source":"linear","kind":"task","title":"New"}]})");
    CHECK(Feed_apply(patch));
    CHECK(!Feed_cacheTick(2000, 30000));  // first sight of the change: the debounce starts
    CHECK(!Feed_cacheTick(20000, 30000)); // too soon
    CHECK(Feed_cacheTick(33000, 30000) && saves == 1);
    CHECK(!Feed_cacheTick(99000, 30000) && saves == 1); // clean again
    FeedDelta same = parseOk(R"({"version":7,"epoch":"e1","items":[{"numeric_id":3,"id":"l:3","source":"linear","kind":"task","title":"New"}]})");
    CHECK(Feed_apply(same));
    CHECK(!Feed_cacheTick(200000, 30000) && saves == 1); // an identical item is not a change
    // what was saved restores the same cache
    Feed_clear();
    CHECK(Feed_restore(last) && Feed_total() == 3 && Feed_version() == 6);
    Feed_setCacheSaver(nullptr);
}

void testLinkProto()
{
    // HMAC-SHA256 against RFC 4231 test cases 1 and 2
    CHECK(LinkProto::toHex(LinkProto::hmacSha256(std::string(20, ''), "Hi There")) ==
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    CHECK(LinkProto::toHex(LinkProto::hmacSha256("Jefe", "what do ya want for nothing?")) ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    // a key longer than the block size is hashed first (RFC 4231 case 6)
    CHECK(LinkProto::toHex(LinkProto::hmacSha256(std::string(131, 'ª'),
                                                 "Test Using Larger Than Block-Size Key - Hash Key First")) ==
          "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");

    // the proof the Python app (pc/dashlink/protocol.py) computes for the same inputs
    CHECK(LinkProto::proof("secret", "00112233445566778899aabbccddeeff", "desk") ==
          "cece2869e341f73926cc74026d3de54f331328e7e2c4edc526df9c694e8f046c");
    CHECK(LinkProto::verifyProof("secret", "00112233445566778899aabbccddeeff", "desk",
                                 "cece2869e341f73926cc74026d3de54f331328e7e2c4edc526df9c694e8f046c"));
    CHECK(!LinkProto::verifyProof("other", "00112233445566778899aabbccddeeff", "desk",
                                  "cece2869e341f73926cc74026d3de54f331328e7e2c4edc526df9c694e8f046c"));
    CHECK(!LinkProto::verifyProof("secret", "00112233445566778899aabbccddeeff", "desk", "cece"));
    CHECK(!LinkProto::verifyProof("secret", "00112233445566778899aabbccddeeff", "desk", ""));

    // framing: encode, then parse the stream in awkward pieces
    const std::string a = LinkProto::encodeFrame(LinkProto::kPing, "{\"t\":5}");
    const std::string b = LinkProto::encodeFrame(LinkProto::kState, "{\"window\":null}");
    const std::string c = LinkProto::encodeFrame(LinkProto::kPong);
    CHECK(a.size() == 4 + 1 + 7 && static_cast<unsigned char>(a[0]) == 8 && a[4] == 'P');
    const std::string stream = a + b + c;
    LinkProto::FrameParser parser;
    char type = 0;
    std::string json;
    int frames = 0;
    for (size_t i = 0; i < stream.size(); ++i) // one byte at a time
    {
        parser.feed(reinterpret_cast<const uint8_t *>(stream.data()) + i, 1);
        while (parser.next(type, json) == LinkProto::FrameParser::Result::Frame)
        {
            ++frames;
            if (frames == 1) CHECK(type == 'P' && json == "{\"t\":5}");
            if (frames == 2) CHECK(type == 'S' && json == "{\"window\":null}");
            if (frames == 3) CHECK(type == 'O' && json.empty());
        }
    }
    CHECK(frames == 3);
    CHECK(parser.next(type, json) == LinkProto::FrameParser::Result::NeedMore);

    // all at once, then a bad length poisons the stream
    LinkProto::FrameParser whole;
    whole.feed(reinterpret_cast<const uint8_t *>(stream.data()), stream.size());
    int count = 0;
    while (whole.next(type, json) == LinkProto::FrameParser::Result::Frame) ++count;
    CHECK(count == 3);
    for (const char *bad : {"    ", "@  S"}) // length 0; length 16385
    {
        LinkProto::FrameParser broken;
        broken.feed(reinterpret_cast<const uint8_t *>(bad), 5);
        CHECK(broken.next(type, json) == LinkProto::FrameParser::Result::Error);
        broken.feed(reinterpret_cast<const uint8_t *>(a.data()), a.size());
        CHECK(broken.next(type, json) == LinkProto::FrameParser::Result::Error); // stays failed until reset
        broken.reset();
        broken.feed(reinterpret_cast<const uint8_t *>(a.data()), a.size());
        CHECK(broken.next(type, json) == LinkProto::FrameParser::Result::Frame);
    }
    // the largest legal frame passes, one byte more does not
    const std::string big = LinkProto::encodeFrame('S', std::string(LinkProto::kMaxFrame - 1, 'x'));
    LinkProto::FrameParser large;
    large.feed(reinterpret_cast<const uint8_t *>(big.data()), big.size());
    CHECK(large.next(type, json) == LinkProto::FrameParser::Result::Frame && json.size() == LinkProto::kMaxFrame - 1);
    const std::string tooBig = LinkProto::encodeFrame('S', std::string(LinkProto::kMaxFrame, 'x'));
    LinkProto::FrameParser huge;
    huge.feed(reinterpret_cast<const uint8_t *>(tooBig.data()), tooBig.size());
    CHECK(huge.next(type, json) == LinkProto::FrameParser::Result::Error);
}

void testSysMon()
{
    CHECK(SysMon_usedPercent(25, 100) == 75 && SysMon_usedPercent(0, 100) == 100 && SysMon_usedPercent(100, 100) == 0);
    CHECK(SysMon_usedPercent(200, 100) == 0);  // free can never exceed total, but do not underflow if it does
    CHECK(SysMon_usedPercent(10, 0) == -1);    // no total, no answer
    CHECK(SysMon_usedPercent(61 * 1024, 330 * 1024) == 82);
}

void testBattery()
{
    CHECK(Battery_percentFromPinMv(2450) == 100 && Battery_percentFromPinMv(2200) == 0);
    CHECK(Battery_percentFromPinMv(2325) == 50);
    CHECK(Battery_percentFromPinMv(3000) == 100 && Battery_percentFromPinMv(100) == 0); // clamped
    CHECK(Battery_millivoltsFromPinMv(2450) == 4200 && Battery_millivoltsFromPinMv(2200) == 3700);
    CHECK(!Battery_state().present); // nothing sampled on the host
}

int main()
{
    testRegistry();
    testMqttp();
    testManifest();
    testFacts();
    testWatch();
    testTap();
    testPing();
    testDeviceView();
    testFeed();
    testCalendar();
    testFeedCache();
    testLinkProto();
    testSysMon();
    testBattery();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
