#pragma once
#include <stdint.h>

// What the board's LED strip says. Sources post an effect under their own name and a priority; the strip shows the
// highest-priority one still posted (the latest of equals), and is dark when none is. The strip is a status light, not a
// lamp: there is no "pick a colour".
//
// Built in: "net" (boot / Wi-Fi connecting: amber comet, then a green sweep once the station has an address) and
// "stackport" (red pulse for a critical alert, amber pulse for warnings) and "calls" (while the PC reports a call: red when
// the microphone is muted, green when live). Another source only needs LedFeedback_set(name, ...) and LedFeedback_clear(name).
//
// LVGL task only (it runs from an lv_timer).

enum class LedKind : uint8_t
{
    Solid, // every pixel `rgb`
    Pulse, // breathes between dim and `rgb` once per `periodMs`
    Blink, // `rgb` for half of `periodMs`, dark for the other half
    Comet, // one bright pixel with a tail travelling along the strip, one lap per `periodMs`
    Sweep, // fills pixel by pixel over `periodMs`, then holds
};

struct LedEffect
{
    LedKind kind = LedKind::Solid;
    uint32_t rgb = 0;
    uint16_t periodMs = 1000;
    uint32_t durationMs = 0; // 0: until cleared; otherwise it clears itself after this long
};

/// Priorities; use any value between them for a new source. Higher wins.
enum LedPriority : uint8_t
{
    LED_PRIO_AMBIENT = 10,
    LED_PRIO_NETWORK = 20,
    LED_PRIO_INFO = 30,
    LED_PRIO_WARNING = 50,
    LED_PRIO_CRITICAL = 60,
    LED_PRIO_CALL = 70, // the call microphone state: while the PC reports a call it outranks everything, alerts included
};

/// Create the 50 ms animation timer. Call once, after LedStrip_begin().
void LedFeedback_begin();

/// Post (or replace) the effect of `source` (up to 11 characters; up to 8 sources). Re-posting the same effect keeps its
/// animation running. False when the table is full.
bool LedFeedback_set(const char *source, const LedEffect &effect, uint8_t priority);
void LedFeedback_clear(const char *source);

/// The user's switch (drawer): off keeps the strip dark whatever is posted. Saved in Prefs.
void LedFeedback_setEnabled(bool enabled);
bool LedFeedback_enabled();
