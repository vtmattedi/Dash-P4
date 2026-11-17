#pragma once
#include "lvgl.h"

// The row of coloured status icons shared by the status bar and the resting screen: Wi-Fi, broker, air conditioner,
// light and door. One definition so the two places can never disagree.
//
//   Wi-Fi    green with an address, amber while connecting, red when it is not there
//   broker   green on MQTT, blue on ESP-NOW, amber while reaching it, red with Wi-Fi but no broker, grey when off
//   AC       blue when running, grey when off, faint when unbound or stale
//   light    yellow when the light or the colour light is on
//   door     red open, green closed, faint without a reading
//   memory   "RAM 78%" and "PSRAM 24%": the share in use (amber from 80 %, red from 90 %); the internal pool is the scarce one

struct StatusIcons
{
    lv_obj_t *row = nullptr;
    lv_obj_t *wifi = nullptr, *mqtt = nullptr, *ac = nullptr, *light = nullptr, *door = nullptr;
    lv_obj_t *ram = nullptr, *psram = nullptr; // the memory monitor: free internal RAM and free PSRAM
};

/// Build the row inside `parent`. `height` is the row's height; the icons are centred in it.
void StatusIcons_create(StatusIcons &icons, lv_obj_t *parent, int32_t height);

/// Recolour everything from the live state. LVGL task only.
void StatusIcons_refresh(const StatusIcons &icons);
