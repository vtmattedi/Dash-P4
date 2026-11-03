#include "Registry.h"

#include <algorithm>
#include <string.h>

#include <ArduinoJson.h>

namespace
{
std::map<std::string, Device> g_devices;
size_t g_resourceTotal = 0;
uint32_t g_revision = 0;
uint32_t g_structure = 0;
uint32_t g_evictedDevices = 0;
uint32_t g_evictedResources = 0;
RegistryPresenceFn g_presence[4];
RegistryValueFn g_valueListeners[4];

void announcePresence(const Device &device, bool firstSeen)
{
    for (RegistryPresenceFn fn : g_presence)
    {
        if (fn)
        {
            fn(device, device.online, firstSeen);
        }
    }
}

void bump(Device &device)
{
    ++device.revision;
    ++g_revision;
}

// Drop the oldest offline device (or, failing that, the oldest of all) to make room.
void evictDevice()
{
    auto victim = g_devices.end();
    for (auto it = g_devices.begin(); it != g_devices.end(); ++it)
    {
        const bool better =
            victim == g_devices.end() ||
            (!it->second.online && victim->second.online) ||
            (it->second.online == victim->second.online &&
             it->second.lastSeenMs < victim->second.lastSeenMs);
        if (better)
        {
            victim = it;
        }
    }
    if (victim != g_devices.end())
    {
        g_resourceTotal -= victim->second.resources.size();
        g_devices.erase(victim);
        ++g_evictedDevices;
        ++g_structure;
    }
}

// Drop the least recently updated resource anywhere.
void evictResource()
{
    Device *owner = nullptr;
    std::map<std::string, ResourceState>::iterator oldest;
    uint32_t oldestMs = UINT32_MAX;
    for (auto &entry : g_devices)
    {
        for (auto it = entry.second.resources.begin(); it != entry.second.resources.end(); ++it)
        {
            if (it->second.lastSeenMs <= oldestMs)
            {
                oldestMs = it->second.lastSeenMs;
                oldest = it;
                owner = &entry.second;
            }
        }
    }
    if (owner)
    {
        owner->resources.erase(oldest);
        ++owner->revision;
        --g_resourceTotal;
        ++g_evictedResources;
    }
}

Device &touch(const std::string &name, uint32_t nowMs)
{
    auto it = g_devices.find(name);
    if (it == g_devices.end())
    {
        if (g_devices.size() >= REGISTRY_MAX_DEVICES)
        {
            evictDevice();
        }
        it = g_devices.emplace(name, Device()).first;
        it->second.name = name;
        it->second.firstSeenMs = nowMs;
        ++g_structure;
    }
    it->second.lastSeenMs = nowMs;
    return it->second;
}

void assignText(std::string &dst, const NetMessage &m)
{
    dst.assign(reinterpret_cast<const char *>(m.payload), m.length);
}

void handleStatus(const NetMessage &m, const std::string &name)
{
    if (m.length == 0)
    {
        // Tombstone: takes an existing device offline but never creates one.
        auto it = g_devices.find(name);
        if (it != g_devices.end() && it->second.online)
        {
            it->second.online = false;
            bump(it->second);
            ++g_structure;
            announcePresence(it->second, false);
        }
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, m.payload, m.length))
    {
        return; // not a status document; ignore rather than guess
    }
    Device &device = touch(name, m.receivedMs);
    const bool firstSeen = !device.seenStatus;
    const bool wasOnline = device.online;
    device.seenStatus = true;
    device.statusMs = m.receivedMs;
    device.online = doc["online"] | false;
    device.hardware = doc["hardware"] | device.hardware.c_str();
    device.timezone = doc["timezone"] | device.timezone.c_str();
    const std::string previousKind = device.kind;
    device.kind = doc["kind"] | "";
    bump(device);
    if (firstSeen || wasOnline != device.online || previousKind != device.kind)
    {
        ++g_structure;
    }
    if (firstSeen || wasOnline != device.online)
    {
        announcePresence(device, firstSeen);
    }
}

void handleResourceState(const NetMessage &m, const std::string &device, const std::string &resource)
{
    Device &owner = touch(device, m.receivedMs);
    auto it = owner.resources.find(resource);
    if (m.length == 0)
    {
        if (it != owner.resources.end() && !it->second.withdrawn)
        {
            it->second.withdrawn = true;
            it->second.lastSeenMs = m.receivedMs;
            bump(owner);
        }
        return;
    }
    if (it == owner.resources.end())
    {
        if (g_resourceTotal >= REGISTRY_MAX_RESOURCES)
        {
            evictResource();
        }
        // evictResource() may have erased from `owner`, so look it up again.
        it = owner.resources.emplace(resource, ResourceState()).first;
        ++g_resourceTotal;
    }
    ResourceState &state = it->second;
    const bool changed = state.withdrawn || state.value.size() != m.length ||
                         memcmp(state.value.data(), m.payload, m.length) != 0;
    if (changed)
    {
        state.value.assign(reinterpret_cast<const char *>(m.payload), m.length);
    }
    state.withdrawn = false;
    state.retained = m.retained;
    state.lastSeenMs = m.receivedMs;
    ++state.updates;
    if (changed)
    {
        bump(owner);
    }
    for (RegistryValueFn fn : g_valueListeners)
    {
        if (fn)
        {
            fn(device, resource, state);
        }
    }
}
} // namespace

std::vector<std::string> Registry_split(const char *topic)
{
    std::vector<std::string> parts;
    const char *start = topic;
    for (const char *p = topic;; ++p)
    {
        if (*p == '/' || *p == '\0')
        {
            parts.emplace_back(start, p - start);
            if (*p == '\0')
            {
                break;
            }
            start = p + 1;
        }
    }
    return parts;
}

void Registry_noteValue(const std::string &device, const std::string &resource, const std::string &value,
                        uint32_t nowMs)
{
    if (device.empty() || resource.empty() || value.empty())
    {
        return;
    }
    NetMessage m{"", reinterpret_cast<const uint8_t *>(value.c_str()), value.size(), false, nowMs};
    handleResourceState(m, device, resource);
}

bool Registry_onValue(RegistryValueFn fn)
{
    for (RegistryValueFn &slot : g_valueListeners)
    {
        if (slot == fn)
        {
            return true;
        }
    }
    for (RegistryValueFn &slot : g_valueListeners)
    {
        if (!slot)
        {
            slot = fn;
            return true;
        }
    }
    return false;
}

bool Registry_onPresence(RegistryPresenceFn fn)
{
    for (RegistryPresenceFn &slot : g_presence)
    {
        if (slot == fn)
        {
            return true;
        }
    }
    for (RegistryPresenceFn &slot : g_presence)
    {
        if (!slot)
        {
            slot = fn;
            return true;
        }
    }
    return false;
}

bool Registry_handle(const NetMessage &m)
{
    const std::vector<std::string> seg = Registry_split(m.topic);
    if (seg.size() < 2 || seg[0].empty() || seg[0] == "Control" || seg[0] == "all")
    {
        return false;
    }
    const std::string &name = seg[0];

    if (seg.size() == 2 && seg[1] == "status")
    {
        handleStatus(m, name);
        return true;
    }
    if (seg.size() == 2 && seg[1] == "info")
    {
        Device &d = touch(name, m.receivedMs);
        assignText(d.infoJson, m);
        bump(d);
        return true;
    }
    if (seg.size() == 3 && seg[1] == "telemetry" && (seg[2] == "system" || seg[2] == "network"))
    {
        Device &d = touch(name, m.receivedMs);
        assignText(seg[2] == "system" ? d.telemetrySystem : d.telemetryNetwork, m);
        bump(d);
        return true;
    }
    if (seg.size() == 3 && seg[1] == "manifest" && seg[2] == "msgpack")
    {
        Device &d = touch(name, m.receivedMs);
        d.manifest.assign(m.payload, m.payload + m.length);
        d.manifestMs = m.receivedMs;
        bump(d);
        return true;
    }
    if (seg.size() == 3 && seg[1] == "gateway" && (seg[2] == "network" || seg[2] == "clients"))
    {
        Device &d = touch(name, m.receivedMs);
        assignText(seg[2] == "network" ? d.gatewayNetwork : d.gatewayClients, m);
        bump(d);
        return true;
    }
    if (seg.size() == 4 && seg[1] == "resource" && seg[3] == "state")
    {
        handleResourceState(m, name, seg[2]);
        return true;
    }
    return false;
}

size_t Registry_deviceCount() { return g_devices.size(); }
size_t Registry_resourceCount() { return g_resourceTotal; }

size_t Registry_onlineCount()
{
    size_t n = 0;
    for (const auto &entry : g_devices)
    {
        n += entry.second.online ? 1 : 0;
    }
    return n;
}

std::vector<const Device *> Registry_devices()
{
    std::vector<const Device *> out;
    out.reserve(g_devices.size());
    for (const auto &entry : g_devices)
    {
        out.push_back(&entry.second);
    }
    return out; // std::map iterates in name order
}

const Device *Registry_find(const std::string &name)
{
    auto it = g_devices.find(name);
    return it == g_devices.end() ? nullptr : &it->second;
}

const ResourceState *Registry_value(const std::string &device, const std::string &resource)
{
    const Device *d = Registry_find(device);
    if (!d)
    {
        return nullptr;
    }
    auto it = d->resources.find(resource);
    return it == d->resources.end() ? nullptr : &it->second;
}

std::string Registry_valueText(const std::string &device, const std::string &resource)
{
    const ResourceState *s = Registry_value(device, resource);
    return s ? s->value : std::string();
}

void Registry_forget(const std::string &name)
{
    auto it = g_devices.find(name);
    if (it != g_devices.end())
    {
        g_resourceTotal -= it->second.resources.size();
        g_devices.erase(it);
        ++g_revision;
        ++g_structure;
    }
}

uint32_t Registry_revision() { return g_revision; }
uint32_t Registry_structureRevision() { return g_structure; }
uint32_t Registry_evictedDevices() { return g_evictedDevices; }
uint32_t Registry_evictedResources() { return g_evictedResources; }
