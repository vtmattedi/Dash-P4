#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

// A device's manifest: what it says it exposes. Parsed from the canonical MessagePack at
// `<device>/manifest/msgpack` (decisions D16):
//
//   [encodingVersion, manifestVersion, resources[]]
//   value  = [0, name, access, type, dependsOn|null, advertiseMs, hardware|null]
//   action = [1, name, arguments[]]            argument = [name, type, required]
//   event  = [2, name, type]
//
// A manifest DESCRIBES; `/state` tells the truth. Nothing here decides whether a value is
// fresh, and a device with no manifest (or one that fails to parse) is still explorable:
// the Explorer falls back to the resources seen publishing state (see DeviceView).

enum class ResKind : uint8_t
{
    Value,
    Action,
    Event
};

enum class ValueType : uint8_t
{
    String = 0,
    Boolean = 1,
    Integer = 2,
    Float = 3,
    Struct = 4,
    Time = 5,
    Colour = 6,
    Unknown = 255
};

const char *ValueType_name(ValueType type);

struct ArgSpec
{
    std::string name;
    ValueType type = ValueType::String;
    bool required = false;
};

struct ResourceSpec
{
    std::string name;
    ResKind kind = ResKind::Value;
    bool writable = false;          // Value: access read_write
    ValueType type = ValueType::Unknown; // Value and Event payload type
    std::string dependsOn;          // Value: the local resource this one mirrors
    int32_t advertiseMs = -1;       // Value: how often the owner re-advertises; -1 disabled
    std::vector<ArgSpec> args;      // Action
    // Hardware policy (Value): present when the resource is backed by a device that can be polled.
    bool hasHardware = false;
    int32_t pollMs = 0;
    uint8_t hwFlags = 0;
    bool hwConnectedKnown = false;
    bool hwConnected = false;
    std::string hwNote;
};

struct Manifest
{
    bool ok = false;
    std::string error;
    int encodingVersion = 0;
    int manifestVersion = 0;
    std::vector<ResourceSpec> resources;

    const ResourceSpec *find(const std::string &name) const;
};

/// Parse MessagePack bytes. Never throws; on failure `ok` is false and `error` says why.
Manifest Manifest_parse(const uint8_t *data, size_t length);
