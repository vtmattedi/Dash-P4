#include "LedFeedback.h"

#include <math.h>
#include <string.h>

#include "LedStrip.h"
#include "Net.h"
#include "Prefs.h"
#include "Link/LinkServer.h"
#include "Stackport.h"
#include "lvgl.h"

namespace
{
constexpr int kSlots = 8;
constexpr int kMaxPixels = 32;

struct Slot
{
    bool used = false;
    char name[12] = {};
    LedEffect fx;
    uint8_t priority = 0;
    uint32_t startMs = 0;
    uint32_t order = 0; // later wins among equal priorities
};

Slot g_slots[kSlots];
uint32_t g_order = 0;
bool g_enabled = true;
bool g_dark = false; // the strip was last told to go dark (avoid rewriting it every tick)

uint32_t nowMs() { return lv_tick_get(); }

uint32_t scale(uint32_t rgb, float k)
{
    if (k < 0)
        k = 0;
    if (k > 1)
        k = 1;
    const uint32_t r = static_cast<uint32_t>(((rgb >> 16) & 0xFF) * k);
    const uint32_t g = static_cast<uint32_t>(((rgb >> 8) & 0xFF) * k);
    const uint32_t b = static_cast<uint32_t>((rgb & 0xFF) * k);
    return (r << 16) | (g << 8) | b;
}

void render(const LedEffect &fx, uint32_t t, uint32_t *px, int n)
{
    const uint32_t period = fx.periodMs ? fx.periodMs : 1000;
    const float phase = static_cast<float>(t % period) / static_cast<float>(period);
    switch (fx.kind)
    {
    case LedKind::Solid:
        for (int i = 0; i < n; ++i)
            px[i] = fx.rgb;
        break;
    case LedKind::Pulse:
    {
        const float k = 0.12f + 0.88f * (0.5f - 0.5f * cosf(phase * 6.2831853f));
        for (int i = 0; i < n; ++i)
            px[i] = scale(fx.rgb, k);
        break;
    }
    case LedKind::Blink:
        for (int i = 0; i < n; ++i)
            px[i] = phase < 0.5f ? fx.rgb : 0;
        break;
    case LedKind::Comet:
    {
        const float head = phase * static_cast<float>(n);
        for (int i = 0; i < n; ++i)
        {
            float d = head - static_cast<float>(i); // how far behind the head this pixel is
            if (d < 0)
                d += static_cast<float>(n);
            px[i] = d < 4.0f ? scale(fx.rgb, 1.0f - d / 4.0f) : 0;
        }
        break;
    }
    case LedKind::Sweep:
    {
        const float progress = t >= period ? 1.0f : static_cast<float>(t) / static_cast<float>(period);
        const float lit = progress * static_cast<float>(n);
        for (int i = 0; i < n; ++i)
            px[i] = scale(fx.rgb, lit - static_cast<float>(i)); // the leading pixel fades in
        break;
    }
    }
}

Slot *find(const char *name)
{
    for (Slot &s : g_slots)
        if (s.used && strncmp(s.name, name, sizeof(s.name) - 1) == 0)
            return &s;
    return nullptr;
}

// --- built-in sources ---------------------------------------------------------------------------------------------

// "net": amber comet while the Wi-Fi station is joining (from boot), a green sweep when it gets an address, then nothing.
void followNetwork()
{
    static bool connectedBefore = false;
    if (!Prefs_get().wifiOn)
    {
        LedFeedback_clear("net");
        connectedBefore = false;
        return;
    }
    if (Net_state().wifiIp)
    {
        if (!connectedBefore)
        {
            connectedBefore = true;
            LedEffect fx;
            fx.kind = LedKind::Sweep;
            fx.rgb = 0x00FF40;
            fx.periodMs = 700;
            fx.durationMs = 1800;
            LedFeedback_set("net", fx, LED_PRIO_NETWORK);
        }
    }
    else
    {
        connectedBefore = false;
        LedEffect fx;
        fx.kind = LedKind::Comet;
        fx.rgb = 0xFF8000;
        fx.periodMs = 1100;
        LedFeedback_set("net", fx, LED_PRIO_NETWORK);
    }
}

// "stackport": critical alerts pulse red (fast), warnings pulse amber (slow); nothing when there are none.
void followStackport()
{
    bool critical = false, warning = false;
    for (const SpAlert &a : Stackport_alerts())
    {
        if (a.severity == "critical")
            critical = true;
        else if (a.severity == "warning")
            warning = true;
    }
    LedEffect fx;
    if (critical)
    {
        fx.kind = LedKind::Pulse;
        fx.rgb = 0xFF0000;
        fx.periodMs = 900;
        LedFeedback_set("stackport", fx, LED_PRIO_CRITICAL);
    }
    else if (warning)
    {
        fx.kind = LedKind::Pulse;
        fx.rgb = 0xFF8000;
        fx.periodMs = 2200;
        LedFeedback_set("stackport", fx, LED_PRIO_WARNING);
    }
    else
    {
        LedFeedback_clear("stackport");
    }
}

// "calls": while the PC reports a call, the strip is the microphone: red when muted, green when live.
void followCalls()
{
    const PcCall call = LinkServer_call();
    if (!call.active)
    {
        LedFeedback_clear("calls");
        return;
    }
    LedEffect fx;
    fx.kind = LedKind::Solid;
    fx.rgb = call.muted ? 0xFF0000 : 0x00C040;
    LedFeedback_set("calls", fx, LED_PRIO_CALL);
}

void tick(lv_timer_t *)
{
    const uint32_t now = nowMs();
    followNetwork();
    followStackport();
    followCalls();

    Slot *top = nullptr;
    for (Slot &s : g_slots)
    {
        if (!s.used)
            continue;
        if (s.fx.durationMs && now - s.startMs >= s.fx.durationMs)
        {
            s.used = false;
            continue;
        }
        if (!top || s.priority > top->priority || (s.priority == top->priority && s.order > top->order))
            top = &s;
    }
    if (!g_enabled || !top)
    {
        if (!g_dark)
        {
            g_dark = LedStrip_off();
        }
        return;
    }
    uint32_t px[kMaxPixels];
    int n = LedStrip_count();
    if (n > kMaxPixels)
        n = kMaxPixels;
    render(top->fx, now - top->startMs, px, n);
    g_dark = false;
    LedStrip_setFrame(px, n);
}
} // namespace

void LedFeedback_begin()
{
    g_enabled = Prefs_get().ledOn;
    lv_timer_create(tick, 50, nullptr);
}

bool LedFeedback_set(const char *source, const LedEffect &effect, uint8_t priority)
{
    Slot *s = find(source);
    if (s)
    {
        const bool same = s->fx.kind == effect.kind && s->fx.rgb == effect.rgb && s->fx.periodMs == effect.periodMs &&
                          s->fx.durationMs == effect.durationMs && s->priority == priority;
        if (same)
            return true; // keep the animation running
    }
    else
    {
        for (Slot &candidate : g_slots)
            if (!candidate.used)
            {
                s = &candidate;
                break;
            }
        if (!s)
            return false;
        s->used = true;
        strncpy(s->name, source, sizeof(s->name) - 1);
        s->name[sizeof(s->name) - 1] = 0;
    }
    s->fx = effect;
    s->priority = priority;
    s->startMs = nowMs();
    s->order = ++g_order;
    return true;
}

void LedFeedback_clear(const char *source)
{
    if (Slot *s = find(source))
        s->used = false;
}

void LedFeedback_setEnabled(bool enabled)
{
    g_enabled = enabled;
    Prefs_setLedOn(enabled);
    g_dark = false; // let the next tick write the strip
}

bool LedFeedback_enabled() { return g_enabled; }
