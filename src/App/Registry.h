#pragma once
#include <stdint.h>
#include <map>
#include <string>
#include <vector>

#include "NetTypes.h"

// What the panel has heard on the network, kept as the model behind the Network
// Explorer and the Home page.
//
// Everything arrives as retained documents on a handful of topic families
// (see .local/decisions.md D16):
//
//   <dev>/status                       presence + identity   {name, hardware, timezone, online}
//   <dev>/info                         boot-scoped description (opaque JSON here)
//   <dev>/telemetry/system|network     runtime health (opaque JSON here)
//   <dev>/manifest/msgpack             what the device exposes (raw bytes here)
//   <dev>/resource/<name>/state        a Value's current state, codec text
//   <dev>/gateway/network|clients      gateway announcements (raw JSON here)
//
// The registry stores what was published and does not interpret it beyond routing;
// parsing a manifest or a telemetry document is the Explorer's job, done on demand
// for the one device being looked at.
//
// Not thread-safe by itself: it belongs to the LVGL task. Any other task must hold
// board_lvgl_lock() while it reads (the console's REGISTRY command does).

/// Capacity. A bounded model on a network the panel does not control: newest wins,
/// and the oldest *offline* device / least recently updated resource is evicted.
#define REGISTRY_MAX_DEVICES 128
#define REGISTRY_MAX_RESOURCES 2048

struct ResourceState
{
    std::string value;         // codec text exactly as published
    uint32_t lastSeenMs = 0;   // ms since boot of the latest publication
    uint32_t updates = 0;      // publications seen since the panel started
    bool retained = false;     // the latest one was a retained replay
    bool withdrawn = false;    // the owner published an empty retained state
};

struct Device
{
    std::string name; // the topic segment: case-sensitive, unique
    std::string hardware;
    std::string timezone;
    std::string kind;      // "gateway" for gateways, empty for ordinary devices
    bool online = false;
    bool seenStatus = false; // false: only heard indirectly (e.g. a resource state)
    uint32_t firstSeenMs = 0;
    uint32_t lastSeenMs = 0; // any message from this device
    uint32_t statusMs = 0;

    // Raw documents, newest only. Parsed on demand by the Explorer.
    std::string infoJson;
    std::string telemetrySystem;
    std::string telemetryNetwork;
    std::string gatewayNetwork;
    std::string gatewayClients;
    std::vector<uint8_t> manifest; // MessagePack
    uint32_t manifestMs = 0;

    std::map<std::string, ResourceState> resources;
    uint32_t revision = 0; // bumped whenever anything above changes
};

/// Called when a device's presence is learned or changes: `firstSeen` is true the first
/// time a status for it is heard (including the retained replay after every reconnect of
/// the panel), `online` is the new state. The policy for what to tell the user lives
/// with the caller. Up to 4 listeners.
using RegistryPresenceFn = void (*)(const Device &device, bool online, bool firstSeen);
bool Registry_onPresence(RegistryPresenceFn fn);

/// Called on every publication of a resource's state (changed or not), after the registry
/// stored it. Up to 4 listeners. The explorer's history recorder uses this.
using RegistryValueFn = void (*)(const std::string &device, const std::string &resource, const ResourceState &state);
bool Registry_onValue(RegistryValueFn fn);

/// Offer a message. True if it belonged to the registry's topic families.
bool Registry_handle(const NetMessage &message);

/// Record a resource value the registry was not shown on the wire. The NightMare resource
/// manager consumes the `/state` of every resource the panel has *bound* (the AC's
/// contract, the light, the colour) before it reaches the app's message callback, so those
/// would be missing from the Explorer exactly when they matter. The owners of those
/// bindings mirror their values back in through here.
void Registry_noteValue(const std::string &device, const std::string &resource, const std::string &value,
                        uint32_t nowMs);

size_t Registry_deviceCount();
size_t Registry_resourceCount();
size_t Registry_onlineCount();

/// All devices, sorted by name. Pointers stay valid until that device is forgotten
/// or evicted.
std::vector<const Device *> Registry_devices();
const Device *Registry_find(const std::string &name);
const ResourceState *Registry_value(const std::string &device, const std::string &resource);

/// The latest published state or "" if never seen.
std::string Registry_valueText(const std::string &device, const std::string &resource);

/// Drop a device and everything held about it. Local only: a live device reappears
/// with its next message.
void Registry_forget(const std::string &name);

/// Bumped on every change anywhere, so screens can redraw only when something moved.
uint32_t Registry_revision();

/// Bumped only when the set of devices, or one's presence/kind, changes -- not for every value.
/// A device list rebuilds on this and merely refreshes labels on Registry_revision().
uint32_t Registry_structureRevision();

uint32_t Registry_evictedDevices();
uint32_t Registry_evictedResources();

/// Split a topic into '/'-separated segments. Exposed for tests and for the Explorer.
std::vector<std::string> Registry_split(const char *topic);
