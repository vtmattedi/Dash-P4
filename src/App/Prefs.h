#pragma once
#include <stdint.h>

// Display preferences, persisted in the NightMare settings store under the same keys the
// older panel used (rest_after, bl_active, bl_rest).
//
// LVGL task only.

struct Prefs
{
    int restAfterSec = 60; // idle seconds before the resting screen; 0 disables it
    int blActive = 200;    // backlight while in use, 8..255
    int blRest = 24;       // backlight while resting, 0..255
    bool pcAuto = true;    // the PC takes the panel as soon as it streams (builds with the PC screen only)
    bool pcTab = true;     // the hand-back tab on the PC's picture (builds with the PC screen only)
};

/// Load from the settings store. Call after introNightMareESP(), which opens it.
void Prefs_load();
const Prefs &Prefs_get();

void Prefs_setRestAfter(int seconds);
void Prefs_setBacklightActive(int level);
void Prefs_setBacklightRest(int level);
void Prefs_setPcAuto(bool on);
void Prefs_setPcTab(bool on);

/// Bumped on every change.
uint32_t Prefs_revision();
