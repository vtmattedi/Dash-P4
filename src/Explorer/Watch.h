#pragma once
#include <stdint.h>
#include <string>
#include <vector>

// History for the resources the user chose to watch, for sparklines in the Explorer.
//
// Only watched resources are recorded: a few KB each, on purpose, rather than a history of
// every value on the network. The watch list is persisted (`explorer_watch`, comma separated
// `device/resource`). Numbers and booleans (as 1/0) are recorded; other values are ignored.
//
// LVGL task only.

struct WatchSample
{
    uint32_t ms; // ms since boot
    float value;
};

#define WATCH_MAX_RESOURCES 12
#define WATCH_MAX_SAMPLES 240
/// A value that did not change is sampled at most this often.
#define WATCH_MIN_INTERVAL_MS 5000

/// Text -> number: decimals, "true"/"false" (1/0). False for anything else.
bool Watch_parseValue(const std::string &text, float &out);

/// Restore the list from its persisted text.
void Watch_restore(const std::string &persisted);
std::string Watch_serialize();

bool Watch_isWatched(const std::string &device, const std::string &resource);
/// Start or stop watching. False if the list is full.
bool Watch_toggle(const std::string &device, const std::string &resource);
size_t Watch_count();

/// Oldest first, or null if not watched.
const std::vector<WatchSample> *Watch_samples(const std::string &device, const std::string &resource);

/// Feed a state publication in (the Registry value listener calls this).
void Watch_record(const std::string &device, const std::string &resource, const std::string &value, uint32_t nowMs);

/// Bumped whenever a sample is added or the list changes.
uint32_t Watch_revision();
