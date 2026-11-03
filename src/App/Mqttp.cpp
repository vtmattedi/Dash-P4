#include "Mqttp.h"

#include <algorithm>
#include <map>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <vector>

namespace
{
constexpr uint32_t kSettleMs = 400;        // wait after a fresh subscribe before the first publish
constexpr uint32_t kLingerMs = 15000;      // keep an unused device subscription this long

struct Request
{
    uint32_t id = 0;
    std::string device;
    std::string command;
    MqttpCallback callback;
    MqttpResult result;
    uint32_t queuedMs = 0;
    uint32_t sentMs = 0;
    uint32_t deadlineMs = 0;
    uint32_t timeoutMs = 0;
    bool sent = false;
    int lastChunk = 0;
};

struct Listener
{
    int refs = 0;
    uint32_t since = 0;     // when the subscription was taken
    uint32_t idleSince = 0; // when refs reached 0
};

MqttpPorts g_ports{};
std::vector<Request> g_requests;
std::map<std::string, Listener> g_listeners;
uint32_t g_nextId = 1;
uint32_t g_stray = 0;
// Callbacks are collected and run after the tables are consistent, so a callback may
// freely start another request.
std::vector<std::pair<MqttpCallback, MqttpResult>> g_done;

uint32_t now() { return g_ports.nowMs ? g_ports.nowMs() : 0; }

std::string replyFilter(const std::string &device)
{
    return device + "/console/controlled/+/out";
}

void finish(Request &r, MqttpStatus status)
{
    r.result.status = status;
    r.result.id = r.id;
    r.result.device = r.device;
    r.result.command = r.command;
    r.result.rttMs = r.sent ? now() - r.sentMs : 0;
    if (r.callback)
    {
        g_done.emplace_back(std::move(r.callback), r.result);
    }
    r.id = 0; // mark for removal
}

void flushDone()
{
    // A callback can start new requests, which touches g_done; work from a copy.
    std::vector<std::pair<MqttpCallback, MqttpResult>> batch;
    batch.swap(g_done);
    for (auto &entry : batch)
    {
        entry.first(entry.second);
    }
}

void sweep()
{
    g_requests.erase(std::remove_if(g_requests.begin(), g_requests.end(),
                                    [](const Request &r) { return r.id == 0; }),
                     g_requests.end());
}

void appendReply(Request &r, const char *data, size_t length)
{
    std::string &out = r.result.response;
    if (out.size() >= MQTTP_RESPONSE_MAX)
    {
        r.result.truncated = true;
        return;
    }
    const size_t room = MQTTP_RESPONSE_MAX - out.size();
    if (length > room)
    {
        out.append(data, room);
        r.result.truncated = true;
    }
    else
    {
        out.append(data, length);
    }
}
} // namespace

void Mqttp_setPorts(const MqttpPorts &ports) { g_ports = ports; }

void Mqttp_listen(const std::string &device)
{
    if (device.empty())
    {
        return;
    }
    Listener &l = g_listeners[device];
    if (l.refs == 0 && l.since == 0)
    {
        l.since = now() ? now() : 1;
        if (g_ports.subscribe)
        {
            g_ports.subscribe(replyFilter(device));
        }
    }
    ++l.refs;
}

void Mqttp_unlisten(const std::string &device)
{
    auto it = g_listeners.find(device);
    if (it == g_listeners.end() || it->second.refs == 0)
    {
        return;
    }
    if (--it->second.refs == 0)
    {
        it->second.idleSince = now();
    }
}

uint32_t Mqttp_request(const std::string &device, const std::string &command, MqttpCallback callback,
                       uint32_t timeoutMs)
{
    if (device.empty() || command.empty() || g_requests.size() >= MQTTP_MAX_PENDING)
    {
        return 0;
    }
    // Be listening before asking. A request-scoped listen: released when it finishes.
    Mqttp_listen(device);

    Request r;
    r.id = g_nextId++;
    r.device = device;
    r.command = command;
    r.callback = std::move(callback);
    r.queuedMs = now();
    r.timeoutMs = timeoutMs;
    r.deadlineMs = r.queuedMs + kSettleMs + timeoutMs;
    g_requests.push_back(std::move(r));
    return g_requests.back().id;
}

void Mqttp_cancel(uint32_t id)
{
    for (Request &r : g_requests)
    {
        if (r.id == id)
        {
            Mqttp_unlisten(r.device);
            finish(r, MqttpStatus::Cancelled);
        }
    }
    sweep();
    flushDone();
}

void Mqttp_tick()
{
    const uint32_t t = now();

    for (Request &r : g_requests)
    {
        if (r.id == 0)
        {
            continue;
        }
        if (!r.sent)
        {
            const Listener &l = g_listeners[r.device];
            if (t - l.since >= kSettleMs)
            {
                const std::string topic = r.device + "/console/controlled/dp4-" + std::to_string(r.id) + "/in";
                r.sentMs = t;
                r.sent = true;
                r.deadlineMs = t + r.timeoutMs;
                if (!g_ports.publish || !g_ports.publish(topic, r.command))
                {
                    Mqttp_unlisten(r.device);
                    r.result.response = "not connected";
                    finish(r, MqttpStatus::Failed);
                }
            }
        }
        else if (static_cast<int32_t>(t - r.deadlineMs) >= 0)
        {
            Mqttp_unlisten(r.device);
            finish(r, MqttpStatus::Timeout);
        }
    }
    sweep();

    // Drop device subscriptions nobody has used for a while.
    for (auto it = g_listeners.begin(); it != g_listeners.end();)
    {
        if (it->second.refs == 0 && t - it->second.idleSince >= kLingerMs)
        {
            if (g_ports.unsubscribe)
            {
                g_ports.unsubscribe(replyFilter(it->first));
            }
            it = g_listeners.erase(it);
        }
        else
        {
            ++it;
        }
    }
    flushDone();
}

bool Mqttp_handleMessage(const NetMessage &m)
{
    // <device>/console/controlled/<id>/out  -- 5 segments, but a device name never
    // contains '/', so locate the marker rather than splitting.
    static const char marker[] = "/console/controlled/";
    const char *at = strstr(m.topic, marker);
    if (!at)
    {
        return false;
    }
    const std::string device(m.topic, at - m.topic);
    const char *idStart = at + sizeof(marker) - 1;
    const char *slash = strchr(idStart, '/');
    if (!slash || strcmp(slash, "/out") != 0)
    {
        return false;
    }
    const std::string id(idStart, slash - idStart);

    // Ids we issue are "dp4-<n>". Anything else is someone else's conversation.
    if (id.rfind("dp4-", 0) != 0)
    {
        return true;
    }
    const uint32_t numeric = static_cast<uint32_t>(strtoul(id.c_str() + 4, nullptr, 10));

    Request *request = nullptr;
    for (Request &r : g_requests)
    {
        // Topic case can be altered on the way; the device name compare is therefore loose.
        if (r.id == numeric && r.id != 0 && strcasecmp(r.device.c_str(), device.c_str()) == 0)
        {
            request = &r;
            break;
        }
    }
    if (!request)
    {
        ++g_stray;
        return true;
    }

    const char *payload = reinterpret_cast<const char *>(m.payload);
    size_t length = m.length;

    // Unchunked: the whole reply in one message.
    if (length < 2 || payload[0] != ';' || payload[1] != ';')
    {
        appendReply(*request, payload, length);
        Mqttp_unlisten(request->device);
        finish(*request, MqttpStatus::Ok);
        sweep();
        flushDone();
        return true;
    }

    // ;;<header>;;<data> -- the data may itself contain ";;", so only the first pair counts.
    const char *headerEnd = nullptr;
    for (size_t i = 2; i + 1 < length; ++i)
    {
        if (payload[i] == ';' && payload[i + 1] == ';')
        {
            headerEnd = payload + i;
            break;
        }
    }
    if (!headerEnd)
    {
        appendReply(*request, payload, length);
        Mqttp_unlisten(request->device);
        finish(*request, MqttpStatus::Ok);
        sweep();
        flushDone();
        return true;
    }

    const std::string header(payload + 2, headerEnd - (payload + 2));
    const char *data = headerEnd + 2;
    const size_t dataLength = length - static_cast<size_t>(data - payload);

    if (header == "error")
    {
        appendReply(*request, data, dataLength);
        Mqttp_unlisten(request->device);
        finish(*request, MqttpStatus::Failed);
        sweep();
        flushDone();
        return true;
    }

    const size_t slashAt = header.find('/');
    if (slashAt == std::string::npos)
    {
        appendReply(*request, data, dataLength);
        Mqttp_unlisten(request->device);
        finish(*request, MqttpStatus::Ok);
        sweep();
        flushDone();
        return true;
    }

    const int current = atoi(header.c_str());
    const int total = atoi(header.c_str() + slashAt + 1);
    appendReply(*request, data, dataLength);
    request->lastChunk = current;
    if (current >= total)
    {
        Mqttp_unlisten(request->device);
        finish(*request, MqttpStatus::Ok);
        sweep();
        flushDone();
    }
    else
    {
        // Each chunk proves the device is still answering: extend, don't time out mid-reply.
        request->deadlineMs = now() + request->timeoutMs;
    }
    return true;
}

uint32_t Mqttp_pendingCount()
{
    uint32_t n = 0;
    for (const Request &r : g_requests)
    {
        n += r.id ? 1 : 0;
    }
    return n;
}

uint32_t Mqttp_now() { return now(); }

uint32_t Mqttp_strayReplies() { return g_stray; }
