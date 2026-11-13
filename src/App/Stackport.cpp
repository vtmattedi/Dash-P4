#include "Stackport.h"

#include <algorithm>
#include <string.h>

#include <ArduinoJson.h>

namespace
{
std::string g_instance;
std::string g_reason, g_snapshotAt;
SpLink g_link = SpLink::None;
SpHost g_host;
std::vector<SpProject> g_projects;
std::vector<SpAction> g_actions;
std::vector<SpAlert> g_alerts;
bool g_hasData = false;
uint32_t g_revision = 0;

// "stackport/<instance>/<rest>": the instance and what follows it, or false.
bool split(const char *topic, std::string &instance, std::string &rest)
{
    static const char kPrefix[] = "stackport/";
    if (strncmp(topic, kPrefix, sizeof(kPrefix) - 1) != 0)
    {
        return false;
    }
    const char *start = topic + sizeof(kPrefix) - 1;
    const char *slash = strchr(start, '/');
    if (!slash || slash == start)
    {
        return false;
    }
    instance.assign(start, slash - start);
    rest.assign(slash + 1);
    return true;
}

std::string text(JsonVariantConst v, size_t max = 80)
{
    const char *s = v.is<const char *>() ? v.as<const char *>() : nullptr;
    return s ? std::string(s, strnlen(s, max)) : std::string();
}

int whole(JsonVariantConst v, int fallback)
{
    return v.is<long>() ? static_cast<int>(v.as<long>()) : fallback;
}

float number(JsonVariantConst v, float fallback)
{
    return v.is<float>() ? v.as<float>() : fallback;
}

int stateRank(const std::string &state)
{
    return state == "down" ? 0 : state == "problem" ? 1 : (state == "paused" || state == "not-deployed") ? 2 : 3;
}

int severityRank(const std::string &severity)
{
    return severity == "critical" ? 0 : severity == "warning" ? 1 : 2;
}

void parseStatus(const char *payload, size_t length)
{
    SpLink link = SpLink::None;
    std::string reason, snapshotAt;
    if (length > 0)
    {
        JsonDocument doc;
        if (!deserializeJson(doc, payload, length) && doc.is<JsonObjectConst>())
        {
            const std::string state = text(doc["state"]);
            link = state == "online"       ? SpLink::Online
                   : state == "connecting" ? SpLink::Connecting
                   : state == "stale"      ? SpLink::Stale
                                           : SpLink::Offline; // "offline", and anything this firmware does not know
            reason = text(doc["reason"]);
            snapshotAt = text(doc["snapshotAt"], 40);
        }
    }
    if (link != g_link || reason != g_reason || snapshotAt != g_snapshotAt)
    {
        g_link = link;
        g_reason = reason;
        g_snapshotAt = snapshotAt;
        ++g_revision;
    }
}

void parseHost(JsonVariantConst v)
{
    SpHost host;
    if (v.is<JsonObjectConst>())
    {
        host.valid = true;
        host.cpuPercent = number(v["cpuPercent"], 0);
        host.memPercent = number(v["memPercent"], 0);
        host.diskUsedPct = number(v["diskUsedPct"], 0);
        host.load1 = number(v["load1"], 0);
        host.memUsedMb = whole(v["memUsedMb"], 0);
        host.memTotalMb = whole(v["memTotalMb"], 0);
        host.netRxBps = number(v["netRxBps"], 0);
        host.netTxBps = number(v["netTxBps"], 0);
    }
    g_host = host;
}

void parseProjects(JsonVariantConst v)
{
    std::vector<SpProject> out;
    for (JsonVariantConst row : v.as<JsonArrayConst>())
    {
        SpProject p;
        p.id = whole(row["id"], 0);
        p.name = text(row["name"]);
        if (p.id <= 0 || p.name.empty())
        {
            continue;
        }
        p.group = text(row["group"]);
        p.state = text(row["state"], 16);
        p.health = text(row["health"], 16);
        p.running = whole(row["containers"]["running"], 0);
        p.total = whole(row["containers"]["total"], 0);
        JsonVariantConst traffic = row["traffic"];
        if (traffic.is<JsonObjectConst>())
        {
            p.requests = whole(traffic["requests"], -1);
            p.errorRatePct = number(traffic["errorRatePct"], -1);
            p.p95Ms = number(traffic["p95Ms"], -1);
            // Under 20 requests the percentages are real numbers that Stackport's own alert rules ignore, and so do we.
            if (p.requests < 20)
            {
                p.errorRatePct = -1;
                p.p95Ms = -1;
            }
        }
        out.push_back(std::move(p));
    }
    std::stable_sort(out.begin(), out.end(), [](const SpProject &a, const SpProject &b) {
        const int ra = stateRank(a.state), rb = stateRank(b.state);
        return ra != rb ? ra < rb : a.name < b.name;
    });
    g_projects = std::move(out);
}

void parseActions(JsonVariantConst v)
{
    std::vector<SpAction> out;
    for (JsonVariantConst row : v.as<JsonArrayConst>())
    {
        SpAction a;
        // The id is a string or a number.
        a.id = row["id"].is<const char *>() ? text(row["id"]) : std::to_string(whole(row["id"], 0));
        a.kind = text(row["kind"], 24);
        a.projectName = text(row["projectName"]);
        a.progress = whole(row["progress"], -1);
        out.push_back(std::move(a));
    }
    g_actions = std::move(out);
}

void parseAlerts(JsonVariantConst v)
{
    std::vector<SpAlert> out;
    for (JsonVariantConst row : v.as<JsonArrayConst>())
    {
        SpAlert a;
        a.id = text(row["id"]);
        if (a.id.empty())
        {
            continue;
        }
        a.severity = text(row["severity"], 16);
        a.code = text(row["code"], 40);
        a.subject = text(row["subject"]);
        a.message = text(row["message"], 160);
        a.projectId = whole(row["projectId"], 0);
        out.push_back(std::move(a));
    }
    std::stable_sort(out.begin(), out.end(), [](const SpAlert &a, const SpAlert &b) {
        return severityRank(a.severity) < severityRank(b.severity);
    });
    g_alerts = std::move(out);
}

void parseState(const std::string &section, const char *payload, size_t length)
{
    // A zero-length retained message clears the topic: the section is empty, not unknown.
    JsonDocument doc;
    if (length > 0 && deserializeJson(doc, payload, length))
    {
        return; // malformed: keep what we had
    }
    JsonVariantConst v = doc.as<JsonVariantConst>();
    if (section == "host")
    {
        parseHost(v);
    }
    else if (section == "projects")
    {
        parseProjects(v);
    }
    else if (section == "actions")
    {
        parseActions(v);
    }
    else if (section == "alerts")
    {
        parseAlerts(v);
    }
    else
    {
        return;
    }
    g_hasData = true;
    ++g_revision;
}
} // namespace

bool Stackport_handle(const NetMessage &message)
{
    std::string instance, rest;
    if (!split(message.topic, instance, rest))
    {
        return false;
    }
    if (g_instance.empty())
    {
        g_instance = instance;
    }
    else if (instance != g_instance)
    {
        return true; // another Stackport: ours since the first one is not mixed with it
    }
    if (rest == "status")
    {
        parseStatus(reinterpret_cast<const char *>(message.payload), message.length);
    }
    else if (rest.compare(0, 6, "state/") == 0)
    {
        parseState(rest.substr(6), reinterpret_cast<const char *>(message.payload), message.length);
    }
    return true;
}

const char *Stackport_linkText(SpLink link)
{
    switch (link)
    {
    case SpLink::Connecting:
        return "connecting";
    case SpLink::Online:
        return "online";
    case SpLink::Stale:
        return "stale";
    case SpLink::Offline:
        return "offline";
    default:
        return "no link";
    }
}

SpLink Stackport_link() { return g_link; }
const std::string &Stackport_reason() { return g_reason; }
const std::string &Stackport_snapshotAt() { return g_snapshotAt; }
const std::string &Stackport_instance() { return g_instance; }
bool Stackport_hasData() { return g_hasData; }
bool Stackport_live() { return g_link == SpLink::Online; }
const SpHost &Stackport_host() { return g_host; }
const std::vector<SpProject> &Stackport_projects() { return g_projects; }
const std::vector<SpAction> &Stackport_actions() { return g_actions; }
const std::vector<SpAlert> &Stackport_alerts() { return g_alerts; }

SpSummary Stackport_summary()
{
    SpSummary s;
    for (const SpProject &p : g_projects)
    {
        if (p.state == "paused" || p.state == "not-deployed")
        {
            ++s.paused;
            continue;
        }
        ++s.counted;
        if (p.state == "down")
        {
            ++s.down;
        }
        else if (p.state == "problem")
        {
            ++s.problem;
        }
        else
        {
            ++s.ready;
        }
    }
    return s;
}

uint32_t Stackport_revision() { return g_revision; }

void Stackport_reset()
{
    g_instance.clear();
    g_reason.clear();
    g_snapshotAt.clear();
    g_link = SpLink::None;
    g_host = SpHost();
    g_projects.clear();
    g_actions.clear();
    g_alerts.clear();
    g_hasData = false;
    ++g_revision;
}
