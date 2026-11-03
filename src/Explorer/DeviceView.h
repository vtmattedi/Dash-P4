#pragma once
#include <stdint.h>
#include <string>
#include <vector>

#include "App/Registry.h"
#include "Manifest.h"

// One device as the Explorer shows it: the manifest (what it says it exposes) merged with
// the registry (what it has actually published). The merge rule is the protocol's own:
//
//     a manifest describes, `/state` tells the truth
//
// so a manifest entry with no state yet is listed as "no value", and a resource that
// publishes state without being in any manifest is listed too (its type guessed from the
// text). A device with no manifest at all is therefore still fully explorable.

struct ResourceRow
{
    std::string name;
    ResKind kind = ResKind::Value;
    bool writable = false;
    ValueType type = ValueType::Unknown;
    bool inManifest = false;
    bool hasValue = false;
    bool withdrawn = false;
    std::string value;
    uint32_t lastSeenMs = 0;
    std::string dependsOn;
    const ResourceSpec *spec = nullptr; // valid while the cached manifest is
};

/// The device's manifest, parsed and cached by the manifest's arrival time. Null when the
/// device has published none; check `ok` for one that failed to parse.
const Manifest *Explorer_manifest(const Device &device);

/// Every resource, manifest first (in manifest order) then the unlisted ones, by name.
std::vector<ResourceRow> Explorer_rows(const Device &device);

/// Best guess of a value's type from its text (values carry no type on the wire).
ValueType Explorer_guessType(const std::string &value);

/// A value for a row: colours as #RRGGBB, long text clipped, floats trimmed.
std::string Explorer_display(ValueType type, const std::string &raw, size_t maxLen = 48);

/// Short label for a row's kind/access: "value", "value rw", "action", "event".
const char *Explorer_kindLabel(const ResourceRow &row);

/// Drop cached manifests (when a device is forgotten).
void Explorer_forgetManifest(const std::string &device);
