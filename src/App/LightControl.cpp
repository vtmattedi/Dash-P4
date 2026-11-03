#include "LightControl.h"

#include <NightMare.h>

#include "Bindings.h"
#include "Registry.h"
#include "esp_timer.h"

namespace
{
// Static storage: the resource manager keeps non-owning pointers for the life of the panel.
RemoteState<bool> light{"light"};
RemoteState<uint32_t> rgb{"rgb"};

std::string g_appliedLightDevice, g_appliedLightResource;
std::string g_appliedRgbDevice, g_appliedRgbResource;
bool g_applied = false;

// The colour and level last chosen here. Not the truth -- the device is -- but what "on"
// should restore and what the wheel should open on, neither of which a published 0 can answer.
uint32_t g_chosenColour = 0xFFFFFF;
uint8_t g_chosenBrightness = 255;

uint8_t channelMax(uint32_t colour)
{
    const uint8_t r = (colour >> 16) & 0xFF;
    const uint8_t g = (colour >> 8) & 0xFF;
    const uint8_t b = colour & 0xFF;
    const uint8_t rg = r > g ? r : g;
    return rg > b ? rg : b;
}

// Scale a colour so its largest channel reads 255: the hue with the brightness taken back out
// of it. Black has no hue to recover.
uint32_t normalise(uint32_t colour)
{
    const uint8_t peak = channelMax(colour);
    if (peak == 0)
    {
        return 0;
    }
    const uint32_t r = (((colour >> 16) & 0xFF) * 255u + peak / 2) / peak;
    const uint32_t g = (((colour >> 8) & 0xFF) * 255u + peak / 2) / peak;
    const uint32_t b = ((colour & 0xFF) * 255u + peak / 2) / peak;
    return (r << 16) | (g << 8) | b;
}

uint32_t scale(uint32_t colour, uint8_t level)
{
    const uint32_t r = (((colour >> 16) & 0xFF) * level + 127) / 255;
    const uint32_t g = (((colour >> 8) & 0xFF) * level + 127) / 255;
    const uint32_t b = ((colour & 0xFF) * level + 127) / 255;
    return (r << 16) | (g << 8) | b;
}

uint32_t published()
{
    return rgb.hasValue() ? (rgb.getValue() & 0x00FFFFFF) : 0;
}

// Fold whatever the owner last reported back into the local choice, so the wheel and the
// slider follow a change made elsewhere -- another panel, a console, the device itself.
void adoptPublished()
{
    const uint32_t value = published();
    if (value == 0)
    {
        return; // Off says nothing about which colour or how bright.
    }
    g_chosenColour = normalise(value);
    g_chosenBrightness = channelMax(value);
}

bool publish(uint32_t colour)
{
    if (!Rgb_bound())
    {
        return false;
    }
    return rgb.setValue(colour & 0x00FFFFFF);
}
} // namespace

void LightControl_begin()
{
    // Bound before a source is known: registered now, subscribed once a source is supplied.
    gResourcesManager.bindResource(&light);
    gResourcesManager.bindResource(&rgb);
}

void LightControl_applyBindings()
{
    const Binding &l = Bindings_get(Role::Light);
    const Binding &c = Bindings_get(Role::Rgb);
    // Each half is retargeted only when its own address moved: setSource() drops everything
    // learned from the old source, and a needless one would blank a card until the broker
    // replayed the retained state.
    if (!g_applied || l.device != g_appliedLightDevice || l.resource != g_appliedLightResource)
    {
        g_appliedLightDevice = l.device;
        g_appliedLightResource = l.resource;
        light.setSource(l.device, l.resource);
    }
    if (!g_applied || c.device != g_appliedRgbDevice || c.resource != g_appliedRgbResource)
    {
        g_appliedRgbDevice = c.device;
        g_appliedRgbResource = c.resource;
        rgb.setSource(c.device, c.resource);
    }
    g_applied = true;
}

void LightControl_mirrorToRegistry()
{
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (Bindings_configured(Role::Light) && light.hasValue())
    {
        Registry_noteValue(Bindings_get(Role::Light).device, Bindings_get(Role::Light).resource,
                           light.getValue() ? "true" : "false", now);
    }
    if (Rgb_bound() && rgb.hasValue())
    {
        Registry_noteValue(Bindings_get(Role::Rgb).device, Bindings_get(Role::Rgb).resource,
                           std::to_string(published()), now);
    }
}

LightState LightControl_state()
{
    if (!Bindings_configured(Role::Light))
    {
        return LIGHT_UNBOUND;
    }
    // Stale means the owner withdrew its retained state; the last value is still readable,
    // but reporting it as fact would be a guess.
    if (!light.hasValue() || light.isStale())
    {
        return LIGHT_UNKNOWN;
    }
    return light.getValue() ? LIGHT_ON : LIGHT_OFF;
}

bool LightControl_toggle()
{
    if (!Bindings_configured(Role::Light))
    {
        return false;
    }
    // With no reading yet "the other state" is undefined; asking for "on" is the useful
    // guess, since that is what someone tapping a dark card wants.
    const bool next = light.hasValue() ? !light.getValue() : true;
    return light.setValue(next);
}

bool Rgb_bound() { return Bindings_configured(Role::Rgb); }

bool Rgb_hasState()
{
    return Rgb_bound() && rgb.hasValue() && !rgb.isStale();
}

bool Rgb_on()
{
    return Rgb_hasState() && published() != 0;
}

uint32_t Rgb_colour()
{
    adoptPublished();
    return g_chosenColour;
}

uint8_t Rgb_brightness()
{
    adoptPublished();
    return g_chosenBrightness;
}

bool Rgb_setColour(uint32_t hex)
{
    const uint32_t hue = normalise(hex & 0x00FFFFFF);
    if (hue == 0)
    {
        // Picking black on the wheel means "off", not a colour of its own.
        return Rgb_setOn(false);
    }
    g_chosenColour = hue;
    return publish(scale(g_chosenColour, g_chosenBrightness));
}

bool Rgb_setBrightness(uint8_t level)
{
    g_chosenBrightness = level;
    // Moving the slider on a light that is off leaves it off and sends nothing: brightness
    // is how bright it will be when it comes on, not a command to switch it on.
    if (!Rgb_on())
    {
        return true;
    }
    return publish(scale(g_chosenColour, level));
}

bool Rgb_setOn(bool on)
{
    if (!on)
    {
        // Remember what it was before going dark, so the next "on" restores it.
        adoptPublished();
        return publish(0);
    }
    // Never switch on into black: a brightness of zero would look like the command did nothing.
    const uint8_t level = g_chosenBrightness ? g_chosenBrightness : 255;
    g_chosenBrightness = level;
    return publish(scale(g_chosenColour, level));
}
