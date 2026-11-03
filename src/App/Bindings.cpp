#include "Bindings.h"

#include <NightMare.h>

#include "Registry.h"

namespace
{
struct RoleInfo
{
    const char *deviceKey;
    const char *resourceKey; // nullptr for a whole-device binding
    const char *invertKey;   // nullptr unless the role has a polarity
    const char *label;
    const char *defaultResource;
};

// Same keys as the older panel, so an upgraded unit keeps its configuration.
const RoleInfo kRoles[static_cast<int>(Role::Count)] = {
    {"ac_host", nullptr, nullptr, "AC", ""},
    {"light_host", "light_res", nullptr, "Light", "light"},
    {"rgb_host", "rgb_res", nullptr, "RGB", "rgb"},
    {"door_device", "door_key", "door_invert", "Door", ""},
    {"rest_device", "rest_key", nullptr, "Resting corner", ""},
};

Binding g_bindings[static_cast<int>(Role::Count)];
uint32_t g_revision = 1;

bool valid(Role role) { return static_cast<int>(role) >= 0 && role < Role::Count; }

const RoleInfo &info(Role role) { return kRoles[static_cast<int>(role)]; }

void persist(Role role)
{
    const RoleInfo &i = info(role);
    const Binding &b = g_bindings[static_cast<int>(role)];
    PersistentSettings.set(i.deviceKey, b.device);
    if (i.resourceKey)
    {
        PersistentSettings.set(i.resourceKey, b.resource);
    }
    if (i.invertKey)
    {
        PersistentSettings.set(i.invertKey, b.invert ? "1" : "0");
    }
}
} // namespace

void Bindings_load()
{
    for (int r = 0; r < static_cast<int>(Role::Count); ++r)
    {
        const RoleInfo &i = kRoles[r];
        Binding &b = g_bindings[r];
        b.device = PersistentSettings.get(i.deviceKey, "");
        b.resource = i.resourceKey ? PersistentSettings.get(i.resourceKey, i.defaultResource) : std::string();
        b.invert = i.invertKey && PersistentSettings.get(i.invertKey, "0") == "1";
    }
    ++g_revision;
}

const Binding &Bindings_get(Role role)
{
    static const Binding empty;
    return valid(role) ? g_bindings[static_cast<int>(role)] : empty;
}

bool Bindings_configured(Role role)
{
    return valid(role) && !g_bindings[static_cast<int>(role)].device.empty();
}

void Bindings_set(Role role, const std::string &device, const std::string &resource, bool invert)
{
    if (!valid(role))
    {
        return;
    }
    Binding &b = g_bindings[static_cast<int>(role)];
    const RoleInfo &i = info(role);
    b.device = device;
    if (b.device.empty())
    {
        b.resource = i.resourceKey ? i.defaultResource : "";
        b.invert = false;
    }
    else
    {
        b.resource = i.resourceKey ? (resource.empty() ? std::string(i.defaultResource) : resource) : std::string();
        b.invert = i.invertKey ? invert : false;
    }
    persist(role);
    ++g_revision;
}

void Bindings_clear(Role role)
{
    Bindings_set(role, std::string());
}

bool Bindings_matches(Role role, const std::string &device, const std::string &resource)
{
    if (!Bindings_configured(role))
    {
        return false;
    }
    const Binding &b = Bindings_get(role);
    if (b.device != device)
    {
        return false;
    }
    return info(role).resourceKey == nullptr || b.resource == resource;
}

std::string Bindings_rolesOf(const std::string &device, const std::string &resource)
{
    std::string out;
    for (int r = 0; r < static_cast<int>(Role::Count); ++r)
    {
        if (Bindings_matches(static_cast<Role>(r), device, resource))
        {
            out += (out.empty() ? "" : ", ") + std::string(kRoles[r].label);
        }
    }
    return out;
}

const char *Bindings_label(Role role) { return valid(role) ? info(role).label : "?"; }
const char *Bindings_defaultResource(Role role) { return valid(role) ? info(role).defaultResource : ""; }
uint32_t Bindings_revision() { return g_revision; }

std::string Bindings_json()
{
    std::string out = "{";
    for (int r = 0; r < static_cast<int>(Role::Count); ++r)
    {
        const Binding &b = g_bindings[r];
        out += std::string(r ? "," : "") + "\"" + kRoles[r].label + "\":{\"device\":\"" + b.device +
               "\",\"resource\":\"" + b.resource + "\"" + (kRoles[r].invertKey ? (b.invert ? ",\"invert\":true" : ",\"invert\":false") : "") + "}";
    }
    return out + "}";
}

int Bindings_doorOpen()
{
    const Binding &b = Bindings_get(Role::Door);
    if (b.device.empty() || b.resource.empty())
    {
        return -1;
    }
    const ResourceState *s = Registry_value(b.device, b.resource);
    if (!s || s->withdrawn)
    {
        return -1;
    }
    bool high;
    if (s->value == "true" || s->value == "1")
    {
        high = true;
    }
    else if (s->value == "false" || s->value == "0")
    {
        high = false;
    }
    else
    {
        return -1; // anything else is "no reading", not a guess
    }
    return (high != b.invert) ? 1 : 0;
}
