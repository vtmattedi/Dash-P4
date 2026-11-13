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

bool g_dirty = false;

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
    ++g_revision;
}

const Prefs &Prefs_get() { return g_prefs; }

void Prefs_setRestAfter(int seconds, bool save)
{
    g_prefs.restAfterSec = clampInt(seconds, 0, 86400);
    g_dirty = true;
    if (save)
    {
        Prefs_commit();
    }
}

void Prefs_setBacklightActive(int level, bool save)
{
    g_prefs.blActive = clampInt(level, 8, 255);
    g_dirty = true;
    if (save)
    {
        Prefs_commit();
    }
}

void Prefs_setBacklightRest(int level, bool save)
{
    g_prefs.blRest = clampInt(level, 0, 255);
    g_dirty = true;
    if (save)
    {
        Prefs_commit();
    }
}

void Prefs_commit()
{
    if (!g_dirty)
    {
        return;
    }
    g_dirty = false;
    store("rest_after", g_prefs.restAfterSec);
    store("bl_active", g_prefs.blActive);
    store("bl_rest", g_prefs.blRest);
}

uint32_t Prefs_revision() { return g_revision; }
