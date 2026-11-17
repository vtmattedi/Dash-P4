#pragma once
#include <stdint.h>
#include <string>
#include <vector>

// A passive ESP-NOW scanner: every frame the C6 hears (from NightMare gateways, other panels, anything on the channel) is
// noted by sender. ESP-NOW has no "scan" request, so this lists who has been heard, with the signal strength of the last
// frame, and for a NightMare BEACON the gateway's id. Needs ESP-NOW enabled (Settings > Network); a build without ESP-NOW
// lists nothing.

struct EspNowPeer
{
    uint8_t mac[6];
    int rssi;           // dBm of the last frame
    int channel;
    uint32_t lastMs;    // lv_tick of the last frame
    uint32_t frames;
    int lastType;       // NightMare FrameType of the last frame, -1 when it is not a NightMare frame
    char gateway[24];   // the id from a BEACON, "" when none was heard
};

/// Start (clear the table and listen) / stop. LVGL task.
void EspNowScan_start();
void EspNowScan_stop();
bool EspNowScan_listening();

/// A copy of what was heard, strongest first. LVGL task.
std::vector<EspNowPeer> EspNowScan_peers();

std::string EspNowScan_macText(const uint8_t mac[6]);
const char *EspNowScan_typeName(int type);
