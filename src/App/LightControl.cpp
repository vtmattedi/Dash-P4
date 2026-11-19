#include "LightControl.h"

#include <NightMare.h>

#include "Bindings.h"
#include "Registry.h"
#include "esp_timer.h"

namespace
{
// Static storage: the resource manager keeps non-owning pointers for the life of the panel.
RemoteState<bool> light{"light"};
// The bed light is a whole device (Sherlock) addressed by two fixed resources; brightness is left to the device.
RemoteState<bool> bedPower{"led:power"};
RemoteState<ColourType> bedColour{"led:color"};
const char *const kLightResource = "light";
const char *const kBedPowerResource = "led:power";
const char *const kBedColourResource = "led:color";

std::string g_appliedLightDevice, g_appliedBedDevice;
bool g_applied = false;

// The colour last chosen here, so "on" and the wheel have something to show before the device has reported one.
uint32_t g_chosenColour = 0xFFFFFF;

uint32_t publishedColour()
{
    const NightMare::RGB c = bedColour.getValue().toRGB();
    return (static_cast<uint32_t>(c.r) << 16) | (static_cast<uint32_t>(c.g) << 8) | c.b;
}
} // namespace

void LightControl_begin()
{
    // Bound before a source is known: registered now, subscribed once a source is supplied.
    gResourcesManager.bindResource(&light);
    gResourcesManager.bindResource(&bedPower);
    gResourcesManager.bindResource(&bedColour);
}

void LightControl_applyBindings()
{
    const std::string &lightDevice = Bindings_get(Role::Light).device;
    const std::string &bedDevice = Bindings_get(Role::Rgb).device;
    // Each half is retargeted only when its own device moved: setSource() drops everything learned from the old
    // source, and a needless one would blank a card until the broker replayed the retained state.
    if (!g_applied || lightDevice != g_appliedLightDevice)
    {
        g_appliedLightDevice = lightDevice;
        light.setSource(lightDevice, kLightResource);
    }
    if (!g_applied || bedDevice != g_appliedBedDevice)
    {
        g_appliedBedDevice = bedDevice;
        bedPower.setSource(bedDevice, kBedPowerResource);
        bedColour.setSource(bedDevice, kBedColourResource);
    }
    g_applied = true;
}

void LightControl_mirrorToRegistry()
{
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (Bindings_configured(Role::Light) && light.hasValue())
    {
        Registry_noteValue(Bindings_get(Role::Light).device, kLightResource, light.getValue() ? "true" : "false", now);
    }
    if (Rgb_bound())
    {
        const std::string &device = Bindings_get(Role::Rgb).device;
        if (bedPower.hasValue())
        {
            Registry_noteValue(device, kBedPowerResource, bedPower.getValue() ? "true" : "false", now);
        }
        if (bedColour.hasValue())
        {
            Registry_noteValue(device, kBedColourResource, std::to_string(publishedColour()), now);
        }
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

bool Rgb_hasState() { return Rgb_bound() && bedPower.hasValue() && !bedPower.isStale(); }

bool Rgb_on() { return Rgb_hasState() && bedPower.getValue(); }

uint32_t Rgb_colour()
{
    if (bedColour.hasValue())
    {
        const uint32_t value = publishedColour();
        if (value != 0)
        {
            g_chosenColour = value;
        }
    }
    return g_chosenColour;
}

bool Rgb_setColour(uint32_t hex)
{
    hex &= 0x00FFFFFF;
    if (hex == 0)
    {
        // Picking black on the wheel means "off", not a colour of its own.
        return Rgb_setOn(false);
    }
    if (!Rgb_bound())
    {
        return false;
    }
    g_chosenColour = hex;
    bool ok = bedColour.setValue(
        ColourType(static_cast<uint8_t>(hex >> 16), static_cast<uint8_t>(hex >> 8), static_cast<uint8_t>(hex)));
    if (!Rgb_on())
    {
        ok = bedPower.setValue(true) && ok;
    }
    return ok;
}

bool Rgb_setOn(bool on) { return Rgb_bound() && bedPower.setValue(on); }
