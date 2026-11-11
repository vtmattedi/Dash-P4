#pragma once
#include <stdint.h>

// Battery gauge for the Guition JC1060P470C.
//
// What is known (the vendor's Arduino demo, 1-Demo/.../adc_test): the battery is sensed through a divider on ADC2
// channel 4 (P4 GPIO53), read at 12 dB attenuation. The demo calls a calibrated pin reading of 2450 mV "full" and
// 2200 mV "empty" and maps that range linearly to 0..100 %. Those two numbers are the vendor's, so the percentage
// follows the vendor; the voltage below is the same scale stretched so that "full" is a 4.20 V cell and "empty" a
// 3.70 V one, which makes it an approximation of the real cell voltage (+-0.1 V), not a measurement.
//
// What is NOT known: the schematic could not be read (it is a PDF), so no charger status pin is wired in. `charging`
// is therefore inferred from the trend of the reading: it is true while the smoothed voltage has been rising over the
// last minute or so, false while it is falling or flat. It lags by about a minute and is wrong when the load changes.
//
// Without a battery connected the divider floats near 0 mV: `present` is then false and the UI hides the gauge.

struct BatteryState
{
    bool present = false;
    bool charging = false;
    int pinMv = 0;       // calibrated voltage at the ADC pin
    int millivolts = 0;  // approximate cell voltage
    int percent = 0;     // 0..100, the vendor's mapping
};

/// Pure helpers (host-tested).
int Battery_percentFromPinMv(int pinMv);
int Battery_millivoltsFromPinMv(int pinMv);

/// Set up the ADC. Safe to call once; on failure the battery is simply reported as not present.
void Battery_begin();

/// Take a reading if the last one is older than a few seconds. Cheap to call from a timer.
void Battery_sample();

BatteryState Battery_state();
