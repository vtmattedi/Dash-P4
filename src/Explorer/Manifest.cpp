#include "Manifest.h"

#include <ArduinoJson.h>

namespace
{
constexpr int kEncodingVersion = 3;

ValueType toType(JsonVariantConst v)
{
    if (!v.is<int>())
    {
        return ValueType::Unknown;
    }
    const int n = v.as<int>();
    return n >= 0 && n <= 6 ? static_cast<ValueType>(n) : ValueType::Unknown;
}
} // namespace

const char *ValueType_name(ValueType type)
{
    switch (type)
    {
    case ValueType::String:
        return "string";
    case ValueType::Boolean:
        return "boolean";
    case ValueType::Integer:
        return "integer";
    case ValueType::Float:
        return "float";
    case ValueType::Struct:
        return "struct";
    case ValueType::Time:
        return "time";
    case ValueType::Colour:
        return "colour";
    default:
        return "?";
    }
}

const ResourceSpec *Manifest::find(const std::string &name) const
{
    for (const ResourceSpec &r : resources)
    {
        if (r.name == name)
        {
            return &r;
        }
    }
    return nullptr;
}

Manifest Manifest_parse(const uint8_t *data, size_t length)
{
    Manifest out;
    if (!data || length == 0)
    {
        out.error = "empty";
        return out;
    }
    JsonDocument doc;
    const DeserializationError err = deserializeMsgPack(doc, data, length);
    if (err)
    {
        out.error = err.c_str();
        return out;
    }
    JsonArrayConst root = doc.as<JsonArrayConst>();
    if (root.isNull() || root.size() < 3 || !root[0].is<int>() || !root[1].is<int>() || !root[2].is<JsonArrayConst>())
    {
        out.error = "not a manifest";
        return out;
    }
    out.encodingVersion = root[0].as<int>();
    out.manifestVersion = root[1].as<int>();
    // Readers that do not recognise the encoding version reject the document (no fallback reader).
    if (out.encodingVersion != kEncodingVersion)
    {
        out.error = "unsupported manifest encoding " + std::to_string(out.encodingVersion);
        return out;
    }

    for (JsonVariantConst item : root[2].as<JsonArrayConst>())
    {
        JsonArrayConst e = item.as<JsonArrayConst>();
        if (e.isNull() || e.size() < 2 || !e[0].is<int>())
        {
            continue; // skip what we cannot read rather than lose the whole device
        }
        ResourceSpec r;
        r.name = e[1] | "";
        if (r.name.empty())
        {
            continue;
        }
        switch (e[0].as<int>())
        {
        case 0: // value
            r.kind = ResKind::Value;
            r.writable = (e[2] | 0) == 1;
            r.type = toType(e[3]);
            if (e[4].is<const char *>())
            {
                r.dependsOn = e[4].as<const char *>();
            }
            r.advertiseMs = e[5] | -1;
            if (e[6].is<JsonArrayConst>())
            {
                JsonArrayConst hw = e[6].as<JsonArrayConst>();
                r.hasHardware = true;
                r.pollMs = hw[0] | 0;
                r.hwFlags = static_cast<uint8_t>(hw[1] | 0);
                if (hw[2].is<bool>())
                {
                    r.hwConnectedKnown = true;
                    r.hwConnected = hw[2].as<bool>();
                }
                if (hw[3].is<const char *>())
                {
                    r.hwNote = hw[3].as<const char *>();
                }
            }
            break;
        case 1: // action
            r.kind = ResKind::Action;
            for (JsonVariantConst a : e[2].as<JsonArrayConst>())
            {
                JsonArrayConst arg = a.as<JsonArrayConst>();
                if (arg.isNull() || arg.size() < 2)
                {
                    continue;
                }
                ArgSpec spec;
                spec.name = arg[0] | "";
                spec.type = toType(arg[1]);
                spec.required = arg[2] | false;
                r.args.push_back(spec);
            }
            break;
        case 2: // event
            r.kind = ResKind::Event;
            r.type = toType(e[2]);
            break;
        default:
            continue;
        }
        out.resources.push_back(std::move(r));
    }
    out.ok = true;
    return out;
}
