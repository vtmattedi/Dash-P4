#include "Prefs.h"

#include <stdlib.h>
#include <string>

#include <NightMare.h>

namespace
{
Prefs g_prefs;
uint32_t g_revision = 1;

int clampInt(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

int readInt(const char *key, int fallback)
{
    const std::string text = PersistentSettings.get(key, "");
    if (text.empty())
    {
        return fallback;
    }
    return atoi(text.c_str());
}

void store(const char *key, int value)
{
    PersistentSettings.set(key, std::to_string(value));
    ++g_revision;
}
} // namespace

void Prefs_load()
{
    g_prefs.restAfterSec = clampInt(readInt("rest_after", 60), 0, 86400);
    g_prefs.blActive = clampInt(readInt("bl_active", 200), 8, 255);
    g_prefs.blRest = clampInt(readInt("bl_rest", 24), 0, 255);
    g_prefs.pcAuto = readInt("pc_auto", 1) != 0;
    g_prefs.pcTab = readInt("pc_tab", 1) != 0;
    g_prefs.camShare = readInt("cam_share", 0) != 0;
    g_prefs.camExposure = clampInt(readInt("cam_ev", 0), -30, 30);
    g_prefs.camBrightness = clampInt(readInt("cam_br", 0), -50, 50);
    g_prefs.camSaturation = clampInt(readInt("cam_sat", 100), 0, 200);
    g_prefs.camContrast = clampInt(readInt("cam_con", 100), 50, 150);
    g_prefs.camWbHold = readInt("cam_wbh", 0) != 0;
    g_prefs.camWarmth = clampInt(readInt("cam_warm", 0), -50, 50);
    ++g_revision;
}

const Prefs &Prefs_get() { return g_prefs; }

void Prefs_setRestAfter(int seconds)
{
    g_prefs.restAfterSec = clampInt(seconds, 0, 86400);
    store("rest_after", g_prefs.restAfterSec);
}

void Prefs_setBacklightActive(int level)
{
    g_prefs.blActive = clampInt(level, 8, 255);
    store("bl_active", g_prefs.blActive);
}

void Prefs_setBacklightRest(int level)
{
    g_prefs.blRest = clampInt(level, 0, 255);
    store("bl_rest", g_prefs.blRest);
}

void Prefs_setPcAuto(bool on)
{
    g_prefs.pcAuto = on;
    store("pc_auto", on ? 1 : 0);
}

void Prefs_setPcTab(bool on)
{
    g_prefs.pcTab = on;
    store("pc_tab", on ? 1 : 0);
}

void Prefs_setCamShare(bool on)
{
    g_prefs.camShare = on;
    store("cam_share", on ? 1 : 0);
}

void Prefs_setCamAdjust(int exposure, int brightness, int saturation, int contrast, bool wbHold, int warmth)
{
    g_prefs.camExposure = clampInt(exposure, -30, 30);
    g_prefs.camBrightness = clampInt(brightness, -50, 50);
    g_prefs.camSaturation = clampInt(saturation, 0, 200);
    g_prefs.camContrast = clampInt(contrast, 50, 150);
    g_prefs.camWbHold = wbHold;
    g_prefs.camWarmth = clampInt(warmth, -50, 50);
    store("cam_ev", g_prefs.camExposure);
    store("cam_br", g_prefs.camBrightness);
    store("cam_sat", g_prefs.camSaturation);
    store("cam_con", g_prefs.camContrast);
    store("cam_wbh", g_prefs.camWbHold ? 1 : 0);
    store("cam_warm", g_prefs.camWarmth);
}

uint32_t Prefs_revision() { return g_revision; }
