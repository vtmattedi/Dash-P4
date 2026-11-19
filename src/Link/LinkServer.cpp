#include "LinkServer.h"

#include <string.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <ArduinoJson.h>
#include <NightMare.h>

#include "App/Diag.h"
#include "App/Heap.h"
#include "App/Ota.h"
#include "LinkProto.h"
#include "App/Net.h"
#include "Version.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "link";

namespace
{
constexpr const char *kTokenKey = "link_token";
constexpr int kHeartbeatS = 5;
constexpr int kSilenceS = 3 * kHeartbeatS;
constexpr int kHelloTimeoutMs = 5000;

std::mutex g_mutex; // guards g_status and g_token
LinkStatus g_status;
std::string g_token;
int64_t g_lastHeardMs = 0;
std::atomic<bool> g_kick{false};
TaskHandle_t g_task = nullptr;

// Actions the panel asks the PC for. The session loop sends what is in the outbox; the PC's RESULT frames land in g_pending.
struct Pending
{
    uint32_t id = 0;
    bool done = false;
    bool ok = false;
    std::string message;
};
constexpr size_t kPendingKept = 8;
std::vector<std::string> g_outbox; // encoded frames, guarded by g_mutex
std::vector<Pending> g_pending;    // newest last, guarded by g_mutex
uint32_t g_nextAction = 1;

// What the PC page shows, guarded by g_mutex.
PcMedia g_media;
PcVitals g_vitals;
std::vector<PcWindow> g_windows;
std::vector<PcBadge> g_badges;
std::vector<PcLauncher> g_launchers;
std::vector<PcRecent> g_recent;
uint32_t g_pcRevision = 0;

int64_t nowMs() { return esp_timer_get_time() / 1000; }

std::string clip(const char *text, size_t limit)
{
    std::string s = text ? text : "";
    if (s.size() > limit)
    {
        s.resize(limit);
    }
    return s;
}

void say(const std::string &message)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status.message = message;
}

bool tokenAcceptable(const std::string &token)
{
    if (token.size() < 4 || token.size() > 32)
    {
        return false;
    }
    for (unsigned char c : token)
    {
        if (c <= 0x20 || c >= 0x7F)
        {
            return false;
        }
    }
    return true;
}

std::string randomToken()
{
    // No 0/O/1/I: it is read off a screen and typed on a PC.
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::string token;
    for (int i = 0; i < 8; ++i)
    {
        token.push_back(alphabet[esp_random() % (sizeof(alphabet) - 1)]);
    }
    return token;
}

std::string randomNonce()
{
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    return LinkProto::toHex(std::string(reinterpret_cast<const char *>(bytes), sizeof(bytes)));
}

bool sendAll(int fd, const std::string &data)
{
    size_t sent = 0;
    while (sent < data.size())
    {
        const int n = send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0)
        {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool sendJson(int fd, char type, JsonDocument &doc)
{
    std::string body;
    serializeJson(doc, body);
    return sendAll(fd, LinkProto::encodeFrame(type, body));
}

bool sendError(int fd, const char *code, const char *message)
{
    JsonDocument doc;
    doc["code"] = code;
    doc["msg"] = message;
    return sendJson(fd, LinkProto::kError, doc);
}

bool sendStatus(int fd, const char *state, const std::string &message = std::string(), int percent = 0)
{
    JsonDocument doc;
    doc["state"] = state;
    doc["pct"] = percent;
    doc["msg"] = message;
    return sendJson(fd, LinkProto::kStatus, doc);
}

enum class Read
{
    Frame,
    Timeout,
    Closed,
    Bad
};

// One frame from the socket, waiting at most `timeoutMs` in total (a frame that arrives in pieces keeps waiting for the
// rest until the deadline). Whatever else arrived stays in the parser for the next call.
Read readFrame(int fd, LinkProto::FrameParser &parser, int timeoutMs, char &type, std::string &json)
{
    uint8_t buffer[1024];
    const int64_t deadline = nowMs() + timeoutMs;
    for (;;)
    {
        switch (parser.next(type, json))
        {
        case LinkProto::FrameParser::Result::Frame:
            return Read::Frame;
        case LinkProto::FrameParser::Result::Error:
            return Read::Bad;
        case LinkProto::FrameParser::Result::NeedMore:
            break;
        }
        const int64_t remaining = deadline - nowMs();
        if (remaining <= 0)
        {
            return Read::Timeout;
        }
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(fd, &readSet);
        timeval timeout{static_cast<time_t>(remaining / 1000), static_cast<suseconds_t>((remaining % 1000) * 1000)};
        const int ready = select(fd + 1, &readSet, nullptr, nullptr, &timeout);
        if (ready < 0)
        {
            return Read::Closed;
        }
        if (ready == 0)
        {
            return Read::Timeout;
        }
        const int n = recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0)
        {
            return Read::Closed;
        }
        parser.feed(buffer, static_cast<size_t>(n));
    }
}

std::string peerAddressOf(int fd)
{
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    char text[16] = "?";
    if (getpeername(fd, reinterpret_cast<sockaddr *>(&address), &length) == 0)
    {
        inet_ntoa_r(address.sin_addr, text, sizeof(text));
    }
    return text;
}

// The PC page's keys of a state frame: each is sent whole when it changes. Caller holds g_mutex.
void parsePcPage(JsonDocument &doc, const std::string &body)
{
    auto sent = [&](const char *key) { return body.find(std::string("\"") + key + "\"") != std::string::npos; };
    if (sent("media"))
    {
        JsonVariantConst media = doc["media"];
        g_media = PcMedia();
        if (media.is<JsonObjectConst>())
        {
            g_media.title = clip(media["title"] | "", 90);
            g_media.artist = clip(media["artist"] | "", 60);
            g_media.app = clip(media["app"] | "", 20);
            g_media.playing = media["playing"] | false;
            g_media.known = !g_media.title.empty() || !g_media.artist.empty();
        }
    }
    JsonVariantConst vitals = doc["vitals"];
    if (vitals.is<JsonObjectConst>())
    {
        g_vitals = PcVitals();
        g_vitals.known = true;
        g_vitals.cpu = vitals["cpu"] | -1;
        g_vitals.ram = vitals["ram"] | -1;
        g_vitals.disk = vitals["disk"] | -1;
        g_vitals.gpu = vitals["gpu"] | -1;
        g_vitals.gpuTemp = vitals["gpu_temp"] | -1;
        g_vitals.battery = vitals["battery"] | -1;
        g_vitals.ramFreeGb = vitals["ram_free"] | -1.0f;
        g_vitals.cpuTemp = vitals["cpu_temp"] | -1;
        g_vitals.temp = vitals["temp"] | -1;
        g_vitals.charging = vitals["charging"] | false;
    }
    JsonVariantConst windows = doc["windows"];
    if (windows.is<JsonArrayConst>())
    {
        g_windows.clear();
        for (JsonVariantConst w : windows.as<JsonArrayConst>())
        {
            PcWindow window;
            window.id = w["id"] | 0u;
            window.app = clip(w["app"] | "", 20);
            window.title = clip(w["title"] | "", 80);
            window.workspace = clip(w["ws"] | "", 80);
            if (window.id && g_windows.size() < 16)
            {
                g_windows.push_back(window);
            }
        }
        ++g_pcRevision;
    }
    JsonVariantConst badges = doc["badges"];
    if (badges.is<JsonArrayConst>())
    {
        g_badges.clear();
        for (JsonVariantConst b : badges.as<JsonArrayConst>())
        {
            PcBadge badge;
            badge.app = clip(b["app"] | "", 20);
            badge.count = b["count"] | 0;
            if (!badge.app.empty() && badge.count > 0 && g_badges.size() < 6)
            {
                g_badges.push_back(badge);
            }
        }
        ++g_pcRevision;
    }
    JsonVariantConst recent = doc["recent"];
    if (recent.is<JsonArrayConst>())
    {
        g_recent.clear();
        for (JsonVariantConst r : recent.as<JsonArrayConst>())
        {
            if (g_recent.size() < 10)
            {
                PcRecent item;
                item.name = clip(r["name"] | "", 32);
                item.where = clip(r["where"] | "", 40);
                if (!item.name.empty())
                {
                    g_recent.push_back(item);
                }
            }
        }
        ++g_pcRevision;
    }
    JsonVariantConst launchers = doc["launchers"];
    if (launchers.is<JsonArrayConst>())
    {
        g_launchers.clear();
        for (JsonVariantConst l : launchers.as<JsonArrayConst>())
        {
            if (g_launchers.size() < 12)
            {
                PcLauncher launcher; // an object, or a bare string from an older companion
                launcher.name = clip(l.is<JsonObjectConst>() ? (l["name"] | "") : (l | ""), 24);
                launcher.icon = clip(l["icon"] | "", 12);
                for (JsonVariantConst choice : l["menu"].as<JsonArrayConst>())
                {
                    if (launcher.menu.size() < 8)
                    {
                        PcChoice c; // an object, or a bare string
                        c.name = clip(choice.is<JsonObjectConst>() ? (choice["name"] | "") : (choice | ""), 24);
                        c.icon = clip(choice["icon"] | "", 12);
                        launcher.menu.push_back(c);
                    }
                }
                g_launchers.push_back(launcher);
            }
        }
        ++g_pcRevision;
    }
}

void clearPeer(const std::string &why)
{
    Diag_mark("PC link ended");
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status.connected = false;
    g_status.callActive = false; // no link, no call to show
    g_status.peer.clear();
    g_status.peerAddress.clear();
    g_status.peerOs.clear();
    g_status.peerApp.clear();
    g_status.caps.clear();
    g_status.window.clear();
    g_media = PcMedia();
    g_vitals = PcVitals();
    g_windows.clear();
    g_badges.clear();
    g_launchers.clear();
    g_recent.clear();
    ++g_pcRevision;
    g_status.heardAgoMs = 0;
    g_status.message = why;
    g_outbox.clear();
    for (Pending &p : g_pending)
    {
        if (!p.done)
        {
            p.done = true;
            p.ok = false;
            p.message = "the PC link was lost";
        }
    }
}

// One PC, from accept to the end of the link.
void runSession(int fd)
{
    LinkProto::FrameParser parser;
    const std::string nonce = randomNonce();
    const std::string address = peerAddressOf(fd);

    {
        JsonDocument challenge;
        challenge["v"] = LinkProto::kVersion;
        challenge["nonce"] = nonce;
        challenge["device"] = gDeviceIdentity.getDeviceName();
        challenge["fw"] = VERSION;
        challenge["elf"] = Ota_runningBuildId(); // which build this is: the PC checks it after a firmware push
        if (!sendJson(fd, LinkProto::kChallenge, challenge))
        {
            return;
        }
    }

    char type = 0;
    std::string body;
    if (readFrame(fd, parser, kHelloTimeoutMs, type, body) != Read::Frame || type != LinkProto::kHello)
    {
        sendError(fd, "protocol", "expected a hello");
        say("a PC at " + address + " did not say hello");
        return;
    }
    JsonDocument hello;
    if (deserializeJson(hello, body))
    {
        sendError(fd, "protocol", "the hello is not JSON");
        say("a PC at " + address + " sent a broken hello");
        return;
    }
    const std::string name = clip(hello["name"] | "", 64);
    std::string token;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        token = g_token;
    }
    if (static_cast<int>(hello["v"] | -1) != LinkProto::kVersion)
    {
        sendError(fd, "version", "this panel speaks DashLink v0");
        say("a PC at " + address + " speaks another protocol version");
        return;
    }
    if (name.empty() || !LinkProto::verifyProof(token, nonce, name, std::string(hello["auth"] | "")))
    {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            ++g_status.rejected;
        }
        sendError(fd, "auth", "bad pairing token");
        say("wrong token from " + address);
        ESP_LOGW(TAG, "refused %s: bad token", address.c_str());
        return;
    }

    std::string caps;
    for (JsonVariantConst capability : hello["caps"].as<JsonArrayConst>())
    {
        if (capability.is<const char *>())
        {
            caps += (caps.empty() ? "" : ",") + clip(capability.as<const char *>(), 24);
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Diag_mark("PC linked");
        g_status.connected = true;
        g_status.peer = name;
        g_status.peerAddress = address;
        g_status.peerOs = clip(hello["os"] | "", 16);
        g_status.peerApp = clip(hello["app"] | "", 32);
        g_status.caps = caps;
        g_status.window.clear();
        ++g_status.sessions;
        g_status.message = "linked";
    }
    g_lastHeardMs = nowMs();
    {
        JsonDocument welcome;
        welcome["heartbeat_s"] = kHeartbeatS;
        if (!sendJson(fd, LinkProto::kWelcome, welcome))
        {
            clearPeer("link lost: could not answer the hello");
            return;
        }
    }
    ESP_LOGI(TAG, "linked to %s (%s, %s)", name.c_str(), address.c_str(), caps.c_str());
    Heap_log("link up");

    int64_t lastSentMs = nowMs();
    std::string why = "link closed";
    bool restartIntoUpdate = false;
    int lastReportedTenth = -1;
    for (;;)
    {
        if (g_kick.exchange(false))
        {
            why = "link dropped here";
            break;
        }
        // Send what the panel wants the PC to do. (200 ms wait below: an action leaves within a fraction of a second.)
        std::vector<std::string> outgoing;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            outgoing.swap(g_outbox);
        }
        bool sendFailed = false;
        for (const std::string &frame : outgoing)
        {
            if (!sendAll(fd, frame))
            {
                sendFailed = true;
                break;
            }
            lastSentMs = nowMs();
        }
        if (sendFailed)
        {
            why = "link lost: could not send to the PC";
            break;
        }
        const Read result = readFrame(fd, parser, 200, type, body);
        const int64_t now = nowMs();
        if (result == Read::Closed)
        {
            why = "link lost: the PC closed it";
            break;
        }
        if (result == Read::Bad)
        {
            sendError(fd, "protocol", "bad frame length");
            why = "link dropped: bad frame";
            break;
        }
        if (result == Read::Frame)
        {
            g_lastHeardMs = now;
            if (type == LinkProto::kData)
            {
                // A piece of the firmware image: raw bytes, so no JSON. The PC keeps sending, so no ping is needed meanwhile.
                std::string reason;
                bool ok = Ota_pushWrite(reinterpret_cast<const uint8_t *>(body.data()), body.size(), reason);
                if (ok && Ota_pushReceived() == Ota_pushSize())
                {
                    ok = Ota_pushEnd(reason);
                    if (ok)
                    {
                        ESP_LOGI(TAG, "update stored; link task stack high-water %u bytes free",
                                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
                        sendStatus(fd, "done", "", 100);
                        why = "the PC pushed a firmware update; restarting into it";
                        restartIntoUpdate = true;
                        break;
                    }
                }
                if (!ok)
                {
                    sendStatus(fd, "failed", reason);
                }
                else
                {
                    const int tenth = Ota_pushSize() ? static_cast<int>(static_cast<uint64_t>(Ota_pushReceived()) * 10 / Ota_pushSize()) : 0;
                    if (tenth != lastReportedTenth)
                    {
                        lastReportedTenth = tenth;
                        sendStatus(fd, "running", "", tenth * 10);
                    }
                }
                lastSentMs = nowMs();
                continue;
            }
            JsonDocument doc;
            const bool parsed = !deserializeJson(doc, body);
            if (type == LinkProto::kUpdate && parsed)
            {
                const std::string target = clip(doc["target"] | "", 8);
                std::string reason;
                if ((target != "p4" && target != "c6") ||
                    !Ota_pushBegin(target == "c6", doc["size"] | 0u, clip(doc["sha256"] | "", 64), reason))
                {
                    sendStatus(fd, "failed", reason.empty() ? "bad update request" : reason);
                }
                else
                {
                    lastReportedTenth = -1;
                    sendStatus(fd, "ready");
                }
                lastSentMs = nowMs();
            }
            else if (type == LinkProto::kPing)
            {
                JsonDocument pong;
                pong["t"] = parsed ? doc["t"] | 0 : 0;
                if (!sendJson(fd, LinkProto::kPong, pong))
                {
                    why = "link lost: could not answer a ping";
                    break;
                }
                lastSentMs = now;
            }
            else if (type == LinkProto::kState && parsed)
            {
                JsonVariantConst window = doc["window"];
                std::lock_guard<std::mutex> lock(g_mutex);
                const char *display = doc["display"] | "";
                if (strcmp(display, "off") == 0)
                {
                    g_status.pcDisplay = 0;
                }
                else if (strcmp(display, "on") == 0)
                {
                    g_status.pcDisplay = 1;
                }
                else if (strcmp(display, "dim") == 0)
                {
                    g_status.pcDisplay = 2;
                }
                if (window.is<JsonObjectConst>())
                {
                    const std::string app = clip(window["app"] | "", 40);
                    const std::string title = clip(window["title"] | "", 120);
                    g_status.window = app.empty() ? title : app + ": " + title;
                }
                else if (body.find("\"window\"") != std::string::npos)
                {
                    g_status.window.clear(); // "window": null: nothing focused
                }
                JsonVariantConst call = doc["call"];
                if (call.is<JsonObjectConst>())
                {
                    g_status.callActive = call["active"] | false;
                    g_status.callMuted = call["muted"] | false;
                    g_status.callApp = clip(call["app"] | "", 24);
                }
                else if (body.find("\"call\"") != std::string::npos)
                {
                    g_status.callActive = false; // "call": null: not in a call
                }
                parsePcPage(doc, body);
            }
            else if (type == LinkProto::kError)
            {
                why = std::string("the PC reported: ") + clip(doc["msg"] | "?", 60);
                break;
            }
            else if (type == LinkProto::kResult && parsed)
            {
                const uint32_t id = doc["id"] | 0;
                std::lock_guard<std::mutex> lock(g_mutex);
                for (Pending &p : g_pending)
                {
                    if (p.id == id)
                    {
                        p.done = true;
                        p.ok = doc["ok"] | false;
                        p.message = clip(doc["msg"] | "", 80);
                    }
                }
            }
            // anything unknown: hearing it was the point
        }
        if (now - g_lastHeardMs > kSilenceS * 1000)
        {
            why = "link lost: the PC went silent";
            break;
        }
        if (now - lastSentMs >= kHeartbeatS * 1000)
        {
            JsonDocument ping;
            ping["t"] = static_cast<uint32_t>(now);
            if (!sendJson(fd, LinkProto::kPing, ping))
            {
                why = "link lost: could not send a ping";
                break;
            }
            lastSentMs = now;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status.heardAgoMs = static_cast<uint32_t>(now - g_lastHeardMs);
    }
    ESP_LOGI(TAG, "%s", why.c_str());
    if (Ota_pushActive())
    {
        Ota_pushAbort("The PC link was lost during the update.");
    }
    clearPeer(why);
    if (restartIntoUpdate)
    {
        vTaskDelay(pdMS_TO_TICKS(800)); // let the "done" reach the PC
        Ota_pushRestart();
    }
}

void serverTask(void *)
{
    for (;;)
    {
        if (!Net_state().wifiIp)
        {
            say("waiting for Wi-Fi");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        const int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (listener < 0)
        {
            say("could not create the socket");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        const int yes = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(LinkProto::kPort);
        if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || listen(listener, 1) != 0)
        {
            say("could not listen on the link port");
            close(listener);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_status.listening = true;
            g_status.port = LinkProto::kPort;
            g_status.ip = WiFi_info().ip;
            g_status.message = "waiting for a PC";
        }
        ESP_LOGI(TAG, "listening on %s:%u", LinkServer_status().ip.c_str(), static_cast<unsigned>(LinkProto::kPort));

        while (Net_state().wifiIp)
        {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(listener, &readSet);
            timeval timeout{1, 0};
            if (select(listener + 1, &readSet, nullptr, nullptr, &timeout) <= 0)
            {
                continue;
            }
            const int client = accept(listener, nullptr, nullptr);
            if (client < 0)
            {
                continue;
            }
            const int on = 1;
            setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
            const timeval sendTimeout{3, 0};
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout));
            g_kick = false;
            runSession(client);
            close(client);
        }
        close(listener);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_status.listening = false;
        }
        clearPeer("Wi-Fi went away");
    }
}
} // namespace

void LinkServer_begin()
{
    if (g_task != nullptr)
    {
        return;
    }
    std::string token = PersistentSettings.get(kTokenKey, "");
    if (!tokenAcceptable(token))
    {
        token = randomToken();
        PersistentSettings.set(kTokenKey, token);
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_token = token;
        g_status.message = "starting";
    }
    // The stack lives in PSRAM (internal RAM is the scarce pool). This task never writes flash, which a PSRAM stack forbids.
    if (xTaskCreateWithCaps(serverTask, "link", 8192, nullptr, 3, &g_task, MALLOC_CAP_SPIRAM) != pdPASS)
    {
        xTaskCreate(serverTask, "link", 10240, nullptr, 3, &g_task);
    }
}

LinkStatus LinkServer_status()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    LinkStatus copy = g_status;
    if (copy.connected)
    {
        copy.heardAgoMs = static_cast<uint32_t>(nowMs() - g_lastHeardMs);
    }
    return copy;
}

std::string LinkServer_token()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_token;
}

bool LinkServer_setToken(const std::string &requested)
{
    const std::string token = requested.empty() ? randomToken() : requested;
    if (!tokenAcceptable(token))
    {
        return false;
    }
    PersistentSettings.set(kTokenKey, token);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_token = token;
    }
    LinkServer_disconnect(); // the linked PC's proof was made with the old token
    return true;
}

void LinkServer_disconnect() { g_kick = true; }

namespace
{
// Queue an action frame for the session loop and start following its result. Caller holds g_mutex and has checked the
// link is up.
uint32_t queueAction(JsonDocument &doc)
{
    const uint32_t id = g_nextAction++;
    doc["id"] = id;
    std::string body;
    serializeJson(doc, body);
    g_outbox.push_back(LinkProto::encodeFrame(LinkProto::kAction, body));
    Pending pending;
    pending.id = id;
    g_pending.push_back(pending);
    if (g_pending.size() > kPendingKept)
    {
        g_pending.erase(g_pending.begin());
    }
    return id;
}
} // namespace

uint32_t LinkServer_openUrl(const std::string &url, const std::string &browser)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_status.connected)
    {
        return 0;
    }
    JsonDocument doc;
    doc["name"] = "open_url";
    doc["args"]["url"] = url;
    doc["args"]["browser"] = browser;
    return queueAction(doc);
}

uint32_t LinkServer_wakePcDisplay()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_status.connected)
    {
        return 0;
    }
    JsonDocument doc;
    doc["name"] = "wake_display";
    return queueAction(doc);
}

uint32_t LinkServer_sleepPcDisplay()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_status.connected)
    {
        return 0;
    }
    JsonDocument doc;
    doc["name"] = "sleep_display";
    return queueAction(doc);
}

uint32_t LinkServer_sleepPc()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_status.connected)
    {
        return 0;
    }
    JsonDocument doc;
    doc["name"] = "sleep_pc";
    return queueAction(doc);
}

uint32_t LinkServer_micMute(const std::string &mode)
{
    if (mode != "mute" && mode != "unmute" && mode != "toggle")
    {
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_status.connected)
    {
        return 0;
    }
    JsonDocument doc;
    doc["name"] = "mic";
    doc["args"]["mode"] = mode;
    return queueAction(doc);
}

PcMedia LinkServer_media()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_media;
}

PcVitals LinkServer_vitals()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_vitals;
}

std::vector<PcWindow> LinkServer_windows()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_windows;
}

std::vector<PcBadge> LinkServer_badges()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_badges;
}

std::vector<PcRecent> LinkServer_recent()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_recent;
}

uint32_t LinkServer_openRecent(const std::string &name)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_status.connected || name.empty())
    {
        return 0;
    }
    JsonDocument doc;
    doc["name"] = "open_recent";
    doc["args"]["name"] = name;
    return queueAction(doc);
}

std::vector<PcLauncher> LinkServer_launchers()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_launchers;
}

uint32_t LinkServer_pcRevision()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_pcRevision;
}

uint32_t LinkServer_mediaKey(const std::string &command)
{
    if (command != "play_pause" && command != "next" && command != "prev")
    {
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_status.connected)
    {
        return 0;
    }
    JsonDocument doc;
    doc["name"] = "media";
    doc["args"]["cmd"] = command;
    return queueAction(doc);
}

uint32_t LinkServer_focusWindow(uint32_t id)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_status.connected || id == 0)
    {
        return 0;
    }
    JsonDocument doc;
    doc["name"] = "focus";
    doc["args"]["id"] = id;
    return queueAction(doc);
}

uint32_t LinkServer_launch(const std::string &name, const std::string &item)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_status.connected || name.empty())
    {
        return 0;
    }
    JsonDocument doc;
    doc["name"] = "launch";
    doc["args"]["name"] = name;
    if (!item.empty())
        doc["args"]["item"] = item;
    return queueAction(doc);
}

PcCall LinkServer_call()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    PcCall call;
    call.active = g_status.connected && g_status.callActive;
    call.muted = g_status.callMuted;
    call.app = g_status.callApp;
    return call;
}

bool LinkServer_connected()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status.connected;
}

int LinkServer_pcDisplay()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status.pcDisplay;
}

LinkResult LinkServer_result(uint32_t id)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    LinkResult result;
    for (const Pending &p : g_pending)
    {
        if (p.id == id)
        {
            result.known = true;
            result.done = p.done;
            result.ok = p.ok;
            result.message = p.message;
        }
    }
    return result;
}
