#pragma once
#include <string>

#include "App/Bindings.h"

// The flows that give a role its device (and resource): pick a device, then -- for roles
// that bind a single resource -- pick which of its resources, guided by what the registry
// has seen it publish. Used from Settings, from the Home cards ("Choose device") and from
// the Explorer's "Use as ..." shortcut.

/// Run the whole flow for `role`, ending in Bindings_set() and a notification.
void Bind_chooseFor(Role role);

/// Skip the device step: the device is already known (e.g. the Explorer opened it).
void Bind_chooseResource(Role role, const std::string &device);

/// Bind directly (the Explorer already knows both halves) after asking for the polarity
/// when the role needs one.
void Bind_apply(Role role, const std::string &device, const std::string &resource);

/// Remove a binding, with a confirmation.
void Bind_unbind(Role role);
