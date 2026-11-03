#include "Bind.h"

#include <algorithm>
#include <stdlib.h>
#include <vector>

#include "App/Notifications.h"
#include "App/Registry.h"
#include "Modal.h"

namespace
{
bool looksBoolean(const std::string &v)
{
    return v == "true" || v == "false" || v == "1" || v == "0";
}

bool looksInteger(const std::string &v)
{
    if (v.empty())
    {
        return false;
    }
    char *end = nullptr;
    strtol(v.c_str(), &end, 10);
    return end && *end == '\0';
}

// Does a resource's last published value fit what the role needs? Values carry no type on
// the wire, so this is the shape of the text -- the same judgement the old panel made.
bool fits(Role role, const std::string &value)
{
    switch (role)
    {
    case Role::Light:
    case Role::Door:
        return looksBoolean(value);
    case Role::Rgb:
        return looksInteger(value);
    case Role::Rest:
        return !value.empty() && value[0] != '{' && value[0] != '[';
    default:
        return true;
    }
}

const char *noun(Role role)
{
    switch (role)
    {
    case Role::Ac:
        return "air conditioner";
    case Role::Light:
        return "light";
    case Role::Rgb:
        return "colour light";
    case Role::Door:
        return "door sensor";
    default:
        return "resting-screen corner";
    }
}

void finish(Role role, const std::string &device, const std::string &resource, bool invert)
{
    Bindings_set(role, device, resource, invert);
    const std::string what = role == Role::Ac ? device : device + " / " + resource;
    Notify(NotifySeverity::Ok, "bindings", std::string(Bindings_label(role)) + " bound", what);
}
} // namespace

void Bind_apply(Role role, const std::string &device, const std::string &resource)
{
    if (role != Role::Door)
    {
        finish(role, device, resource, false);
        return;
    }
    // Reed switches report either polarity depending only on how they were wired, so ask.
    FormField choice;
    choice.kind = FormField::Kind::Choice;
    choice.key = "polarity";
    choice.label = "A reading of 1 / true means the door is...";
    choice.choices = {"open", "closed"};
    choice.value = "open";
    Modal_form("Door sensor polarity", {choice}, "Bind", Tone::Accent,
               [role, device, resource](const FormValues &v) -> std::string {
                   finish(role, device, resource, v.at("polarity") == "closed");
                   return "";
               });
}

void Bind_chooseResource(Role role, const std::string &device)
{
    const Device *d = Registry_find(device);
    if (!d)
    {
        Modal_message("Unknown device", device + " is not in the registry any more.", Tone::Warn);
        return;
    }
    struct Candidate
    {
        std::string name, value;
        bool fit;
    };
    std::vector<Candidate> candidates;
    for (const auto &entry : d->resources)
    {
        candidates.push_back({entry.first, entry.second.value, fits(role, entry.second.value)});
    }
    const bool anyFit = std::any_of(candidates.begin(), candidates.end(), [](const Candidate &c) { return c.fit; });
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate &a, const Candidate &b) { return a.fit && !b.fit; });

    std::vector<ListEntry> entries;
    for (const Candidate &c : candidates)
    {
        ListEntry e;
        e.label = c.name;
        e.detail = c.value.substr(0, 60);
        e.tone = c.fit ? Tone::Ok : Tone::Neutral;
        entries.push_back(e);
    }
    std::string title = "Which of " + device + "'s resources is the " + noun(role) + "?";
    std::string empty = device + " has published no resources yet.";
    if (!anyFit && !entries.empty())
    {
        title += " (none look like a match)";
    }
    Modal_list(title, entries,
               [role, device, candidates](size_t index) {
                   if (index < candidates.size())
                   {
                       Bind_apply(role, device, candidates[index].name);
                   }
               },
               empty, ModalSize::Wide);
}

void Bind_chooseFor(Role role)
{
    std::vector<const Device *> devices = Registry_devices();
    // Online devices first, then by name; gateways are not controllable things.
    devices.erase(std::remove_if(devices.begin(), devices.end(),
                                 [](const Device *d) { return d->kind == "gateway"; }),
                  devices.end());
    std::stable_sort(devices.begin(), devices.end(),
                     [](const Device *a, const Device *b) { return a->online && !b->online; });

    std::vector<ListEntry> entries;
    std::vector<std::string> names;
    for (const Device *d : devices)
    {
        ListEntry e;
        e.label = d->name;
        e.detail = std::to_string(d->resources.size()) + " resources" + (d->online ? "" : "  -  offline");
        e.tone = d->online ? Tone::Ok : Tone::Neutral;
        entries.push_back(e);
        names.push_back(d->name);
    }
    Modal_list(std::string("Which device is the ") + noun(role) + "?", entries,
               [role, names](size_t index) {
                   if (index >= names.size())
                   {
                       return;
                   }
                   if (role == Role::Ac)
                   {
                       finish(role, names[index], "", false);
                   }
                   else
                   {
                       Bind_chooseResource(role, names[index]);
                   }
               },
               "No devices heard yet. They show up as soon as the broker replays their status.");
}

void Bind_unbind(Role role)
{
    Modal_confirm(std::string("Unbind ") + Bindings_label(role) + "?",
                  std::string("The panel will stop controlling the ") + noun(role) + ".", "Unbind", Tone::Danger,
                  [role] {
                      Bindings_clear(role);
                      Notify(NotifySeverity::Info, "bindings", std::string(Bindings_label(role)) + " unbound");
                  });
}
