#pragma once
#include <stdint.h>

// Display preferences, persisted in the NightMare settings store under the same keys the
// older panel used (rest_after, bl_active, bl_rest).
//
// LVGL task only.

/// Which NightMare transport the panel runs, independent of whether Wi-Fi is on (ESP-NOW needs only the radio, MQTT needs
/// the Wi-Fi station). The two are exclusive: ESP-NOW alone has no MQTT fallback.
enum NetMode : int
{
    NET_MODE_MQTT = 0,
    NET_MODE_ESPNOW = 1,
    NET_MODE_NONE = 2,
};

struct Prefs
{
    // MQTT until the C6's ESP-NOW bridge has been proven on the board; ESP-NOW is one tap away in Settings > Network.
    int netMode = NET_MODE_MQTT;
    bool wifiOn = true; // the Wi-Fi station (joining the access point); off leaves the radio to ESP-NOW
    bool ledOn = true;     // the board LED strip shows status feedback (App/LedFeedback)
    int restAfterSec = 60; // idle seconds before the resting screen; 0 disables it
    int blActive = 200;    // backlight while in use, 8..255
    int blRest = 24;       // backlight while resting, 0..255
};

/// Load from the settings store. Call after introNightMareESP(), which opens it.
void Prefs_load();
const Prefs &Prefs_get();

// The setters take effect at once. With save = false they stay in memory only: a slider calls them on every step (a flash write
// per step stalled the whole UI) and Prefs_commit() on release writes what changed.
void Prefs_setRestAfter(int seconds, bool save = true);
void Prefs_setBacklightActive(int level, bool save = true);
void Prefs_setBacklightRest(int level, bool save = true);
/// One of NetMode; saved at once.
void Prefs_setNetMode(int mode);
void Prefs_setWifiOn(bool on);
void Prefs_setLedOn(bool on);
/// Write whatever was set with save = false.
void Prefs_commit();

/// Bumped on every change.
uint32_t Prefs_revision();
