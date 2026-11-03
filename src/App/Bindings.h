#pragma once
#include <stdint.h>
#include <string>

// Which devices -- and which of their resources -- this panel drives or watches.
//
// The dashboard owns no hardware of its own. Every capability on the network is a
// resource address `<device>/resource/<name>`, so a role is bound to a pair, not to a
// device. The pairs are runtime configuration persisted in the NightMare settings
// store, so a panel is re-pointed from the screen or the console without a reflash.
// The keys are the ones the older devices/Dashboard used, so a migrated unit keeps
// what it was told.
//
//   Ac     a whole device: the AC controller publishes a contract (ac_power,
//          ac_temperature, ac_target, ac_state, ...) under names it chose, so naming
//          the device addresses all of them. resource is unused.
//   Light  one writable boolean resource.
//   Rgb    one writable 24-bit colour resource.
//   Door   a boolean resource that says whether a door is open. Reed switches report
//          either polarity depending only on wiring, hence `invert`.
//   Rest   the resource shown in the resting screen's free corner.
//
// LVGL task only (decisions D20). Nothing here talks to the network: AcClient and
// LightControl re-point their Remote resources when Bindings_revision() moves.

enum class Role : uint8_t
{
    Ac = 0,
    Light,
    Rgb,
    Door,
    Rest,
    Count
};

struct Binding
{
    std::string device;
    std::string resource;
    bool invert = false; // Door only: a reading of 1/true means *closed*
};

/// Load from the settings store. Call after introNightMareESP(), which opens it.
void Bindings_load();

const Binding &Bindings_get(Role role);
bool Bindings_configured(Role role);

/// Bind a role and persist it. An empty device unbinds. `resource` is ignored for Ac.
void Bindings_set(Role role, const std::string &device, const std::string &resource = std::string(),
                  bool invert = false);
void Bindings_clear(Role role);

/// Whether this exact address fills the role.
bool Bindings_matches(Role role, const std::string &device, const std::string &resource);
/// Every role this address fills (Ac matches any resource of the bound device).
std::string Bindings_rolesOf(const std::string &device, const std::string &resource);

const char *Bindings_label(Role role);
const char *Bindings_defaultResource(Role role);

/// Bumped whenever a binding changes.
uint32_t Bindings_revision();

/// All bindings as JSON.
std::string Bindings_json();

/// Door state from the registry: 1 open, 0 closed, -1 no reading.
int Bindings_doorOpen();
