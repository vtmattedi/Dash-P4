#include "DeviceView.h"

#include <algorithm>
#include <map>
#include <stdio.h>
#include <stdlib.h>

namespace
{
struct Cached
{
    uint32_t manifestMs = 0;
    size_t size = 0;
    Manifest manifest;
};
std::map<std::string, Cached> g_cache;

bool isInteger(const std::string &v)
{
    if (v.empty())
    {
        return false;
    }
    char *end = nullptr;
    strtol(v.c_str(), &end, 10);
    return end && *end == '\0';
}

bool isFloat(const std::string &v)
{
    if (v.empty())
    {
        return false;
    }
    char *end = nullptr;
    strtod(v.c_str(), &end);
    return end && *end == '\0';
}
} // namespace

const Manifest *Explorer_manifest(const Device &device)
{
    if (device.manifest.empty())
    {
        g_cache.erase(device.name);
        return nullptr;
    }
    Cached &c = g_cache[device.name];
    if (c.manifestMs != device.manifestMs || c.size != device.manifest.size() || c.manifestMs == 0)
    {
        c.manifest = Manifest_parse(device.manifest.data(), device.manifest.size());
        c.manifestMs = device.manifestMs;
        c.size = device.manifest.size();
    }
    return &c.manifest;
}

void Explorer_forgetManifest(const std::string &device) { g_cache.erase(device); }

ValueType Explorer_guessType(const std::string &v)
{
    if (v == "true" || v == "false")
    {
        return ValueType::Boolean;
    }
    if (isInteger(v))
    {
        return ValueType::Integer;
    }
    if (isFloat(v))
    {
        return ValueType::Float;
    }
    if (!v.empty() && (v[0] == '{' || v[0] == '['))
    {
        return ValueType::Struct;
    }
    return ValueType::String;
}

std::vector<ResourceRow> Explorer_rows(const Device &device)
{
    std::vector<ResourceRow> rows;
    const Manifest *manifest = Explorer_manifest(device);
    std::map<std::string, bool> listed;

    auto fill = [&](ResourceRow &row) {
        auto it = device.resources.find(row.name);
        if (it != device.resources.end())
        {
            row.hasValue = !it->second.withdrawn;
            row.withdrawn = it->second.withdrawn;
            row.value = it->second.value;
            row.lastSeenMs = it->second.lastSeenMs;
            if (row.type == ValueType::Unknown)
            {
                row.type = Explorer_guessType(row.value);
            }
        }
    };

    if (manifest && manifest->ok)
    {
        for (const ResourceSpec &spec : manifest->resources)
        {
            ResourceRow row;
            row.name = spec.name;
            row.kind = spec.kind;
            row.writable = spec.writable;
            row.type = spec.type;
            row.dependsOn = spec.dependsOn;
            row.inManifest = true;
            row.spec = &spec;
            if (spec.kind == ResKind::Value)
            {
                fill(row);
            }
            listed[spec.name] = true;
            rows.push_back(std::move(row));
        }
    }
    // Published without being declared (or no manifest at all).
    for (const auto &entry : device.resources)
    {
        if (listed.count(entry.first))
        {
            continue;
        }
        ResourceRow row;
        row.name = entry.first;
        row.kind = ResKind::Value;
        fill(row);
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string Explorer_display(ValueType type, const std::string &raw, size_t maxLen)
{
    if (type == ValueType::Colour && isInteger(raw))
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "#%06X", (unsigned)(strtoul(raw.c_str(), nullptr, 10) & 0xFFFFFF));
        return buf;
    }
    if (type == ValueType::Float && isFloat(raw))
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%g", strtod(raw.c_str(), nullptr));
        return buf;
    }
    std::string out = raw;
    for (char &c : out)
    {
        if (c == '\n' || c == '\r' || c == '\t')
        {
            c = ' ';
        }
    }
    if (out.size() > maxLen)
    {
        out.resize(maxLen - 1);
        out += "...";
    }
    return out;
}

const char *Explorer_kindLabel(const ResourceRow &row)
{
    switch (row.kind)
    {
    case ResKind::Action:
        return "action";
    case ResKind::Event:
        return "event";
    default:
        return row.writable ? "value rw" : "value";
    }
}
