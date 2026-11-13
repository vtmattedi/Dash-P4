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
};

/// Load from the settings store. Call after introNightMareESP(), which opens it.
void Prefs_load();
const Prefs &Prefs_get();

// The setters take effect at once. With save = false they stay in memory only: a slider calls them on every step (a flash write
// per step stalled the whole UI) and Prefs_commit() on release writes what changed.
void Prefs_setRestAfter(int seconds, bool save = true);
void Prefs_setBacklightActive(int level, bool save = true);
void Prefs_setBacklightRest(int level, bool save = true);
/// Write whatever was set with save = false.
void Prefs_commit();

/// Bumped on every change.
uint32_t Prefs_revision();
