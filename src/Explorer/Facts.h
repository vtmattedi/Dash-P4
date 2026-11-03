#pragma once
#include <stdint.h>
#include <string>

#include "App/Registry.h"

// What a device says about itself, pulled out of its retained documents (docs/protocols/
// status-info.md): `/info` (identity, hardware, build, boot) and `/telemetry/system|network`.
// Every field is optional on the wire, so every group carries a `has…` flag and nothing
// is shown that was not published.

struct DeviceFacts
{
    // /status + /info identity
    std::string name, hardware, id, timezone;

    // /info hardware
    bool hasHardware = false;
    std::string board, chip;
    int cores = 0, revision = 0;
    uint32_t flashBytes = 0, heapBytes = 0, psramBytes = 0;

    // /info build
    bool hasBuild = false;
    std::string firmware, buildDate, buildTime, idfVersion;

    // /info boot
    bool hasBoot = false;
    int resetReason = -1; // esp_reset_reason()

    // /telemetry/system
    bool hasSystem = false;
    uint32_t uptimeMs = 0, cpuMhz = 0, freeHeap = 0, minFreeHeap = 0, freePsram = 0;

    // /telemetry/network
    bool hasNetwork = false;
    std::string transportPreferred, transportActive, transportState;
    std::string wifiState, ssid, ip;
    int rssi = 0, channel = 0;
    std::string mqttState, mqttProfile, espNowState;
    bool gatewayConnected = false;
    int rttMs = -1;
};

DeviceFacts Facts_parse(const Device &device);

/// "1d 02h 03m" style duration.
std::string Facts_formatDuration(uint32_t ms);
/// "3.9 MB" / "512 KB".
std::string Facts_formatBytes(uint32_t bytes);
/// esp_reset_reason_t as words.
const char *Facts_resetReason(int reason);
