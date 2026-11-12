#include "LinkServer.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <ArduinoJson.h>
#include <NightMare.h>

#include "App/Heap.h"
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

void clearPeer(const std::string &why)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status.connected = false;
    g_status.peer.clear();
    g_status.peerAddress.clear();
    g_status.peerOs.clear();
    g_status.peerApp.clear();
    g_status.caps.clear();
    g_status.window.clear();
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
            JsonDocument doc;
            const bool parsed = !deserializeJson(doc, body);
            if (type == LinkProto::kPing)
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
    clearPeer(why);
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
        xTaskCreate(serverTask, "link", 8192, nullptr, 3, &g_task);
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

uint32_t LinkServer_openUrl(const std::string &url, const std::string &browser)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_status.connected)
    {
        return 0;
    }
    const uint32_t id = g_nextAction++;
    JsonDocument doc;
    doc["id"] = id;
    doc["name"] = "open_url";
    doc["args"]["url"] = url;
    doc["args"]["browser"] = browser;
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
