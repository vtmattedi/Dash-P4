#include "AcClient.h"

#include <NightMare.h>

#include "Bindings.h"
#include "Registry.h"
#include "esp_timer.h"

namespace
{
// Resource names as the AC controller publishes them. Fixed rather than bound one by one
// like the light and the colour: the AC is not one capability but a contract of nine, and
// asking for each pairing by hand would be a worse question than "which device is the AC".
const char *const ResState = "ac_state";
const char *const ResKnown = "ac_known";
const char *const ResSleepDeadline = "ac_sleep_deadline";
const char *const ResRoomTemperature = "temperature";
const char *const ResPower = "ac_power";
const char *const ResUnitTemperature = "ac_temperature";
const char *const ResTarget = "ac_target";
const char *const ActManualSync = "ac_manual_sync";
const char *const ActSleep = "ac_sleep";

// What the thermostat comes back to when it has never had a target: the controller's own default.
const float DefaultTarget = 24.0f;

// Static storage: the resource manager keeps non-owning pointers to these for the life of the panel.
RemoteSensor<int8_t> acState{ResState};
RemoteSensor<bool> acKnown{ResKnown};
RemoteSensor<uint32_t> acSleepDeadline{ResSleepDeadline};
RemoteSensor<float> acRoomTemperature{ResRoomTemperature};
RemoteState<bool> acPower{ResPower};
RemoteState<uint8_t> acUnitTemperature{ResUnitTemperature};
RemoteState<float> acTarget{ResTarget};

// Schemas the panel expects, not a claim to mirror the controller's: a mismatch is logged
// by the library and changes nothing.
const ActionArgMetadata ManualSyncArgs[] = {
    {"power", NetValueType::BOOLEAN, true},
    {"temperature", NetValueType::INTEGER, true},
};
const ActionArgMetadata SleepArgs[] = {
    {"minutes", NetValueType::INTEGER, true},
};

RemoteAction acManualSync(ActManualSync, ManualSyncArgs);
RemoteAction acSleep(ActSleep, SleepArgs);

// The device every resource was last pointed at. A revision bump for some other role must
// not tear down subscriptions: setSource() drops everything learned from the old source,
// including retained state that would then have to be replayed.
std::string g_appliedDevice;
bool g_applied = false;

bool hasState()
{
    // Freshness lives on `ac_state`, which the controller republishes on every change, so
    // it is the one resource worth measuring the whole card against.
    return acState.hasValue() && !acState.isStale();
}

int clampUnit(int temperature)
{
    if (temperature < AC_UNIT_TEMP_MIN)
    {
        return AC_UNIT_TEMP_MIN;
    }
    return temperature > AC_UNIT_TEMP_MAX ? AC_UNIT_TEMP_MAX : temperature;
}

std::string fmt(float v)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
    return buf;
}

float clampTarget(float target)
{
    if (target < static_cast<float>(AC_UNIT_TEMP_MIN))
    {
        return static_cast<float>(AC_UNIT_TEMP_MIN);
    }
    return target > static_cast<float>(AC_UNIT_TEMP_MAX) ? static_cast<float>(AC_UNIT_TEMP_MAX) : target;
}
} // namespace

void AcClient_begin()
{
    // Bound before a source is known: the manager registers them now and subscribes as soon
    // as AcClient_applyBindings() supplies an address. Binding this early also matters for
    // identity cleanup, which can only withdraw resources the firmware has already declared.
    gResourcesManager.bindResource(&acState);
    gResourcesManager.bindResource(&acKnown);
    gResourcesManager.bindResource(&acSleepDeadline);
    gResourcesManager.bindResource(&acRoomTemperature);
    gResourcesManager.bindResource(&acPower);
    gResourcesManager.bindResource(&acUnitTemperature);
    gResourcesManager.bindResource(&acTarget);
    gResourcesManager.bindResource(&acManualSync);
    gResourcesManager.bindResource(&acSleep);
}

void AcClient_applyBindings()
{
    const std::string &device = Bindings_get(Role::Ac).device;
    if (g_applied && device == g_appliedDevice)
    {
        return;
    }
    g_appliedDevice = device;
    g_applied = true;

    // An empty device detaches each resource: it then matches no topic, keeps its
    // registration, and can be pointed somewhere valid later.
    acState.setSource(device, ResState);
    acKnown.setSource(device, ResKnown);
    acSleepDeadline.setSource(device, ResSleepDeadline);
    acRoomTemperature.setSource(device, ResRoomTemperature);
    acPower.setSource(device, ResPower);
    acUnitTemperature.setSource(device, ResUnitTemperature);
    acTarget.setSource(device, ResTarget);
    acManualSync.setSource(device, ActManualSync);
    acSleep.setSource(device, ActSleep);
}

void AcClient_mirrorToRegistry()
{
    if (!AcClient_bound())
    {
        return;
    }
    const std::string &device = Bindings_get(Role::Ac).device;
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (acState.hasValue())
    {
        Registry_noteValue(device, ResState, std::to_string(static_cast<int>(acState.getValue())), now);
    }
    if (acKnown.hasValue())
    {
        Registry_noteValue(device, ResKnown, acKnown.getValue() ? "true" : "false", now);
    }
    if (acSleepDeadline.hasValue())
    {
        Registry_noteValue(device, ResSleepDeadline, std::to_string(acSleepDeadline.getValue()), now);
    }
    if (acRoomTemperature.hasValue())
    {
        Registry_noteValue(device, ResRoomTemperature, fmt(acRoomTemperature.getValue()), now);
    }
    if (acPower.hasValue())
    {
        Registry_noteValue(device, ResPower, acPower.getValue() ? "true" : "false", now);
    }
    if (acUnitTemperature.hasValue())
    {
        Registry_noteValue(device, ResUnitTemperature, std::to_string(static_cast<int>(acUnitTemperature.getValue())), now);
    }
    if (acTarget.hasValue())
    {
        Registry_noteValue(device, ResTarget, fmt(acTarget.getValue()), now);
    }
}

#pragma region "Reading"

bool AcClient_bound()
{
    return Bindings_configured(Role::Ac);
}

bool AcClient_isStale()
{
    return !AcClient_bound() || !hasState();
}

AcState AcClient_state()
{
    if (!hasState())
    {
        return AC_UNKNOWN;
    }
    // The controller may believe the unit's state is unknown even while its own state
    // machine reports a value, so that is reported as unknown too.
    if (acKnown.hasValue() && !acKnown.getValue())
    {
        return AC_UNKNOWN;
    }
    return static_cast<AcState>(acState.getValue());
}

bool AcClient_powerOn()
{
    return acPower.hasValue() && acPower.getValue();
}

int AcClient_unitTemperature()
{
    return acUnitTemperature.hasValue() ? static_cast<int>(acUnitTemperature.getValue()) : 0;
}

bool AcClient_targetEnabled()
{
    return acTarget.hasValue() && acTarget.getValue() >= 0.0f;
}

float AcClient_target()
{
    if (!acTarget.hasValue())
    {
        return 0.0f;
    }
    const float value = acTarget.getValue();
    return value < 0 ? -value : value;
}

bool AcClient_roomTemperature(float &out)
{
    // The controller only publishes `temperature` when it owns the sensor. On a board whose
    // reference is another device's reading it is remote there too and never appears under the
    // controller's name -- so an absent value is normal, not a fault.
    if (!acRoomTemperature.hasValue() || acRoomTemperature.isStale())
    {
        return false;
    }
    out = acRoomTemperature.getValue();
    return true;
}

uint32_t AcClient_sleepDeadline()
{
    return acSleepDeadline.hasValue() ? acSleepDeadline.getValue() : 0;
}

#pragma endregion

#pragma region "Commanding"

bool AcClient_togglePower()
{
    if (!AcClient_bound())
    {
        return false;
    }
    return acPower.setValue(!AcClient_powerOn());
}

bool AcClient_toggleTarget()
{
    if (!AcClient_bound())
    {
        return false;
    }
    // Negating keeps the magnitude, which is how the controller remembers the target to
    // resume at. With nothing reported yet there is nothing to negate, so the first tap
    // enables the controller's own default.
    const float current = acTarget.hasValue() ? acTarget.getValue() : -DefaultTarget;
    return acTarget.setValue(-current);
}

bool AcClient_stepTarget(float delta)
{
    if (!AcClient_bound())
    {
        return false;
    }
    const float magnitude =
        clampTarget(AcClient_target() > 0 ? AcClient_target() + delta : DefaultTarget + delta);
    // Stepping adjusts the target without switching the thermostat on or off, so the sign is
    // carried over rather than recomputed.
    return acTarget.setValue(AcClient_targetEnabled() ? magnitude : -magnitude);
}

bool AcClient_stepUnitTemperature(int delta)
{
    if (!AcClient_bound())
    {
        return false;
    }
    const int current = AcClient_unitTemperature();
    const int next = clampUnit((current > 0 ? current : (AC_UNIT_TEMP_MIN + AC_UNIT_TEMP_MAX) / 2) + delta);
    return acUnitTemperature.setValue(static_cast<uint8_t>(next));
}

bool AcClient_manualSync(int temperature, bool power)
{
    if (!AcClient_bound())
    {
        return false;
    }
    char payload[64];
    snprintf(payload, sizeof(payload), "{\"power\":%s,\"temperature\":%d}", power ? "true" : "false",
             clampUnit(temperature));
    return acManualSync.invoke(payload);
}

bool AcClient_setSleepMinutes(uint32_t minutes)
{
    if (!AcClient_bound())
    {
        return false;
    }
    char payload[48];
    snprintf(payload, sizeof(payload), "{\"minutes\":%lu}", static_cast<unsigned long>(minutes));
    return acSleep.invoke(payload);
}

#pragma endregion
