#pragma once
#include <stdint.h>

// The two device roles:
//
//   Role::Light   a device's `light` boolean (its `door` sensor is read by Bindings_doorOpen)
//   Role::Rgb     the bed light: a device's `led:power` boolean and `led:color` colour
//
// Both are bound by device only; the resource names are fixed. The library owns the subscription, the decoding, the
// freshness and the optimistic window; this module is only the panel's reading of what the values mean. Brightness is
// left to the device.

enum LightState
{
    LIGHT_UNBOUND = 0, // no resource bound to the light role
    LIGHT_UNKNOWN,     // bound, but the owner has reported nothing (recently)
    LIGHT_ON,
    LIGHT_OFF
};

/// Declare both resources to the resource manager. Call once, before startNightMareESP().
void LightControl_begin();
/// Point both at what is bound. Cheap when nothing changed.
void LightControl_applyBindings();
/// Copy both values into the registry (see Registry_noteValue).
void LightControl_mirrorToRegistry();

LightState LightControl_state();
/// Ask the bound device for the other state. False if nothing is bound / no transport.
bool LightControl_toggle();

bool Rgb_bound();
bool Rgb_hasState();
bool Rgb_on();
/// The chosen colour at full brightness, for the swatch and the wheel.
uint32_t Rgb_colour();
bool Rgb_setColour(uint32_t hex);
bool Rgb_setOn(bool on);
