#pragma once
#include <stdint.h>

// The air conditioner bound to Role::Ac.
//
// The AC controller publishes a fixed contract of NightMare resources, and this is the
// panel's client for it:
//
//   ac_state            int8    sensor  the controller's own verdict, below
//   ac_known            bool    sensor  whether it believes it knows the unit
//   ac_sleep_deadline   uint32  sensor  epoch second the unit switches off, 0 none
//   temperature         float   sensor  the room reference it controls against
//   ac_power            bool    state   the unit's power
//   ac_temperature      uint8   state   the unit's own setpoint, 18..30
//   ac_target           float   state   room target; negative disables the thermostat
//   ac_manual_sync      action  {"power":bool,"temperature":int}
//   ac_sleep            action  {"minutes":int}, 0 cancels
//
// Every one is declared without a source and pointed at the bound device by
// AcClient_applyBindings(), so re-binding the role is a retarget rather than a rebuild: the
// library drops what it learned from the old device, re-subscribes, and the new device's
// retained state arrives by itself.
//
// Writes are optimistic. Tapping power flips the card immediately and the owner's retained
// `ac_power` wins again once it arrives -- or once the optimistic window closes, which is
// how a command that went nowhere becomes visible.
//
// Threading: the values are scalars written by the library's task when owner state
// arrives and read by the LVGL task to draw; each is one aligned load/store, so nothing
// here can tear. No std::string is consumed, for the same reason.

/// The controller's own state machine, as published on `ac_state`.
enum AcState : int8_t
{
    AC_OFF_TARGET_DOOR_OPEN = -4,
    AC_OFF_DOOR_OPEN = -3,
    AC_OFF_TARGET = -2,
    AC_OFF = -1,
    AC_UNKNOWN = 0,
    AC_ON = 1,
    AC_ON_TARGET = 2,
};

/// The setpoint range the unit itself accepts.
#define AC_UNIT_TEMP_MIN 18
#define AC_UNIT_TEMP_MAX 30

/// Declare the contract to the resource manager. Call once, before startNightMareESP(),
/// so a pending identity cleanup can see it.
void AcClient_begin();

/// Point every resource at the bound device. Cheap when nothing changed.
void AcClient_applyBindings();

/// Copy the AC's current values into the registry (see Registry_noteValue).
void AcClient_mirrorToRegistry();

bool AcClient_bound();
/// True when the controller has not reported recently, or never has.
bool AcClient_isStale();
AcState AcClient_state();
bool AcClient_powerOn();
/// The unit's own setpoint, or 0 when none has been reported.
int AcClient_unitTemperature();
/// Whether the room-temperature thermostat is running. A negative `ac_target` is how the
/// controller reports it switched off, the magnitude remembering the target to return to.
bool AcClient_targetEnabled();
/// The room target, always positive. 0 when none has been reported.
float AcClient_target();
/// The room temperature the controller works from; false when none (recently).
bool AcClient_roomTemperature(float &out);
/// Epoch second the unit is due to switch itself off, or 0.
uint32_t AcClient_sleepDeadline();

// Every command returns false when nothing was sent (no device bound, or no transport).
// None is an acknowledgement; that arrives as new owner state, or does not.
bool AcClient_togglePower();
bool AcClient_toggleTarget();
bool AcClient_stepTarget(float delta);
bool AcClient_stepUnitTemperature(int delta);
/// Tell the controller what the unit is already doing after it was changed behind its
/// back (its own IR remote). Corrects a belief; does not change the room.
bool AcClient_manualSync(int temperature, bool power);
/// Arm the unit's sleep timer. 0 cancels it.
bool AcClient_setSleepMinutes(uint32_t minutes);
