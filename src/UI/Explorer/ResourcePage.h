#pragma once
#include <string>

// One resource of one device, in a modal: its live value and metadata, an editor if it is a
// writable Value, a generated form if it is an Action, the watch history, and the shortcuts
// that give it a job on this panel ("Use as light" ...).

void ResourcePage_open(const std::string &device, const std::string &resource);

/// Publish a Value write for `<device>/resource/<resource>/set` (codec text, not retained).
bool Resource_set(const std::string &device, const std::string &resource, const std::string &payload);
/// Publish an Action invocation for `<device>/resource/<resource>/invoke`.
bool Resource_invoke(const std::string &device, const std::string &resource, const std::string &payloadJson);
