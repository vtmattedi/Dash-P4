#pragma once

// Application wiring: what runs on the LVGL task, and what the console can ask.

/// Call once with the LVGL lock held (after board_init()). Creates the timer that
/// drains the network inbox on the LVGL task, installs the console commands and
/// the network callbacks. Call before startNightMareESP().
void App_begin();

/// Call once, after introNightMareESP() has opened the settings store and before
/// startNightMareESP(): loads the preferences and bindings and declares the resources the
/// panel binds (the AC's contract, the light, the colour). Takes the LVGL lock itself.
void App_loadSettings();
