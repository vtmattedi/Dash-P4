#pragma once
#include <stddef.h>
#include <stdint.h>

// The one network type the app model needs, kept free of the NightMare headers so the
// model (Registry, Mqttp, Notifications) can be compiled and tested on the host.

struct NetMessage
{
    const char *topic;      // NUL-terminated
    const uint8_t *payload; // NUL-terminated for convenience; `length` excludes the NUL
    size_t length;
    bool retained;
    uint32_t receivedMs; // ms since boot when handed to the app (Net_drain, on the LVGL task)
};
