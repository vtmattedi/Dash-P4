#pragma once

// The dashboard's side of the PC screen (env jc1060p470c_usbscreen_nm, DASH_PC_SCREEN): starts PcLink with the
// panel's preferences, keeps the dashboard out of the way while the PC owns the panel, and reports the state
// on the network (resource `pc_screen`), in notifications and on the console (`PCSCREEN`).

#include <NightMare.h>

/// Call once, after App_loadSettings() (it reads the preferences). Takes the LVGL lock itself.
void PcScreen_begin();

/// One line for Settings, e.g. "Streaming - on the panel now". LVGL task.
const char *PcScreen_summary();

/// Console:
///   PCSCREEN                 link, owner, frame counters (JSON)
///   PCSCREEN SHOW | HIDE | CLOSE   the PC's picture (reconnects if closed) / back to the dashboard, connection kept /
///                            back to the dashboard and off the USB bus (the PC loses the display)
///   PCSCREEN AUTO [ON|OFF]   whether the PC takes the panel as soon as it streams (persisted)
/// Any task; takes the LVGL lock.
NightMareResults PcScreen_command(const NightMareMessage &message);
