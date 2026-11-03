#pragma once
#include <stdint.h>

// The two single-resource roles: a plain light and a colour light.
//
//   Role::Light   a writable boolean resource   RemoteState<bool>
//   Role::Rgb     a writable 24-bit colour      RemoteState<uint32_t>
//
// Neither has a contract the way the AC does, so both halves of the address are bound
// from a resource's own page in the Explorer ("Use as light"). The library owns the
// subscription, the decoding, the freshness and the optimistic window; this module is only
// the panel's reading of what the value means.
//
// # Brightness, out of one number
//
// A colour resource is one 24-bit value, so "off", "which colour" and "how bright" all live
// in it. They are read back out as:
//
//   off         the published value is 0
//   brightness  the largest of its three channels
//   colour      the value scaled back up so that channel reads 255
//
// The hue and level last chosen are kept here too, so switching off and on again restores
// them rather than guessing, and so the colour wheel opens on the colour that was picked
// rather than on a dimmed version of it.

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
/// 0-255, from the published colour's largest channel.
uint8_t Rgb_brightness();
bool Rgb_setColour(uint32_t hex);
bool Rgb_setBrightness(uint8_t level);
bool Rgb_setOn(bool on);
