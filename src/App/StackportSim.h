#pragma once
#include <stdint.h>

// Pretend Stackport said something, to see how the rest screen looks with it, without touching the broker. The fake data
// goes through the tracker as the instance "sim": the real instance's messages are then ignored (the tracker follows one
// instance), and stopping forgets it all so the real retained state can be fetched again (see App.cpp's SPSIM command).
//
//   down     a project down, one unhealthy, three alerts
//   warn     everything up, two warnings (latency, disk)
//   run      everything up, a deploy that runs from 0 to the end and finishes
//   all      down + run
//   offline  all up but the bridge reports the source gone: the card must say "last known"
//
// The host numbers wobble every second so the arcs move. LVGL task only.

/// Start a scenario ("down", "warn", "run", "all", "offline"). False for an unknown name. With `loop` the run starts over a
/// few seconds after it ends, so it can be left on a wall to look at.
bool StackportSim_start(const char *scenario, bool loop = false);
/// Stop and forget the fake data.
void StackportSim_stop();
bool StackportSim_active();
/// Advance the fake runs and the wobble. Call every ~100 ms with a millisecond clock.
void StackportSim_tick(uint32_t nowMs);
