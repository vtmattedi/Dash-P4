#include "Prefs.h"

#include "Diag.h"
#include "esp_timer.h"

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

// Every save rewrites the whole settings file on SPIFFS (a temp file, a rename, a delete): seconds of flash work during which
// the UI waits and the display can glitch. So a value that is already stored is not written again, and several values go out
// in one save (store_many).
void store(const char *key, int value)
{
    const std::string text = std::to_string(value);
    if (PersistentSettings.get(key, "") == text)
    {
        ++g_revision;
        return;
    }
    const int64_t start = esp_timer_get_time();
    PersistentSettings.set(key, text);
    Diag_mark((std::string("flash write: ") + key + " took " + std::to_string((esp_timer_get_time() - start) / 1000) + " ms").c_str());
    ++g_revision;
}

void storeMany(const char *const *keys, const int *values, size_t count)
{
    std::string texts[4];
    std::string names[4];
    bool changed = false;
    for (size_t i = 0; i < count && i < 4; ++i)
    {
        names[i] = keys[i];
        texts[i] = std::to_string(values[i]);
        changed = changed || PersistentSettings.get(keys[i], "") != texts[i];
    }
    ++g_revision;
    if (!changed)
    {
        return;
    }
    const int64_t start = esp_timer_get_time();
    PersistentSettings.setMany(names, texts, count);
    Diag_mark((std::string("flash write: display settings, one save, took ") + std::to_string((esp_timer_get_time() - start) / 1000) + " ms").c_str());
}
} // namespace

void Prefs_load()
{
    g_prefs.restAfterSec = clampInt(readInt("rest_after", 60), 0, 86400);
    g_prefs.blActive = clampInt(readInt("bl_active", 200), 8, 255);
    g_prefs.blRest = clampInt(readInt("bl_rest", 24), 0, 255);
    // The old single "net_mode" (0 MQTT, 1 ESP-NOW, 2 none) becomes the two switches until they have been saved once.
    const int legacy = readInt("net_mode", 0);
    g_prefs.mqttOn = readInt("mqtt_on", legacy == 2 ? 0 : 1) != 0;
    g_prefs.espNowOn = readInt("espnow_on", legacy == 1 ? 1 : 0) != 0;
#if !NM_NETWORK_ESPNOW
    g_prefs.espNowOn = false; // this build has no ESP-NOW
#endif
    g_prefs.ledOn = readInt("led_on", 1) != 0;
    ++g_revision;
}

void Prefs_setMqttOn(bool on)
{
    g_prefs.mqttOn = on;
    store("mqtt_on", on ? 1 : 0);
}

void Prefs_setEspNowOn(bool on)
{
    g_prefs.espNowOn = on;
    store("espnow_on", on ? 1 : 0);
}

void Prefs_setLedOn(bool on)
{
    g_prefs.ledOn = on;
    store("led_on", on ? 1 : 0);
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
    static const char *const kKeys[] = {"rest_after", "bl_active", "bl_rest"};
    const int values[] = {g_prefs.restAfterSec, g_prefs.blActive, g_prefs.blRest};
    storeMany(kKeys, values, 3);
}

uint32_t Prefs_revision() { return g_revision; }
