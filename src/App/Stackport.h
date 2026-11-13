#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

#include "NetTypes.h"

// The Stackport tracker: what the panel knows about the server's projects, from the stackport-wss-mqtt-bridge.
//
// The bridge keeps five retained topics under stackport/<instance>/ (see its docs/v0-design.md):
//
//   status          {"state":"connecting|online|stale|offline","reason":..,"observedAt":..,"snapshotAt":..}
//   state/host      the host object (cpu, memory, disk, network) or null
//   state/projects  the complete array of projects
//   state/actions   the complete array of running actions (deploys, restarts ...)
//   state/alerts    the complete array of conditions that hold now
//
// Each state topic is one whole section, so a message replaces what the panel had; there is nothing to merge. The panel
// trusts the numbers as current only while the status says "online"; in any other state they are the last known ones and
// every view says so. The first instance seen is the one followed.
//
// Subscribe to kStackportStatusFilter and kStackportStateFilter, and give every message to Stackport_handle(). LVGL task
// only (the same task that drains the network inbox).

constexpr const char *kStackportStatusFilter = "stackport/+/status";
constexpr const char *kStackportStateFilter = "stackport/+/state/+";

enum class SpLink : uint8_t
{
    None,       // nothing heard from the bridge
    Connecting, // the bridge has no complete snapshot yet
    Online,     // synchronized and the source is healthy
    Stale,      // the source stopped sending
    Offline     // the source or the bridge itself is gone
};

struct SpHost
{
    bool valid = false;
    float cpuPercent = 0, memPercent = 0, diskUsedPct = 0, load1 = 0;
    int memUsedMb = 0, memTotalMb = 0;
    double netRxBps = 0, netTxBps = 0;
};

struct SpProject
{
    int id = 0;
    std::string name, group;
    std::string state;  // ready, problem, down, not-deployed, paused
    std::string health; // as sent ("healthy", "unhealthy", "none" ...)
    int running = 0, total = 0;
    int requests = -1;     // last 5 minutes; -1 = no traffic data
    float errorRatePct = -1; // -1 = unknown
    float p95Ms = -1;        // -1 = unknown
};

struct SpAction
{
    std::string id, kind, projectName;
    int progress = -1; // percent, -1 = unknown
};

struct SpAlert
{
    std::string id, severity, code, subject, message; // severity: critical, warning, info
    int projectId = 0;
};

struct SpSummary
{
    size_t counted = 0; // projects that are supposed to be running (not paused, not undeployed)
    size_t ready = 0;
    size_t problem = 0; // running, health check failing
    size_t down = 0;
    size_t paused = 0; // paused or not deployed
};

/// Handle a message if it belongs to the tracker. Returns true when it did (whether or not it parsed).
bool Stackport_handle(const NetMessage &message);

/// "no link", "connecting", "online", "stale", "offline".
const char *Stackport_linkText(SpLink link);
SpLink Stackport_link();
/// The bridge's reason code ("socket_closed", "stale_data" ...), empty when there is none.
const std::string &Stackport_reason();
/// When the last complete snapshot was taken (ISO 8601 from the bridge), empty if unknown.
const std::string &Stackport_snapshotAt();
/// The Stackport instance being followed ("sp-0765d8101d"), empty until a message arrives.
const std::string &Stackport_instance();
/// Any state section has been received (the retained state arrives within a second of subscribing).
bool Stackport_hasData();
/// True when the numbers are current: the link is Online.
bool Stackport_live();

const SpHost &Stackport_host();
/// Worst first (down, problem, then paused / undeployed, then ready), by name within a state.
const std::vector<SpProject> &Stackport_projects();
const std::vector<SpAction> &Stackport_actions();
/// Critical first.
const std::vector<SpAlert> &Stackport_alerts();
SpSummary Stackport_summary();

/// Bumped whenever anything above changed.
uint32_t Stackport_revision();
/// Forget everything (the tests; a change of broker).
void Stackport_reset();
