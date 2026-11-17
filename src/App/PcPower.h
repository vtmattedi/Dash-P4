#pragma once

// Turning the linked PC off, in the two senses that goes by, for this panel and for everything else on the network.
//
// This panel is the only thing with a link to the PC (Link/LinkServer.h). So besides using it from its own drawer, it
// offers it to the NightMare network as three resources of its own:
//
//   pc_linked         bool sensor   a PC is paired and the link is up
//   pc_monitors_off   action        turn the PC's monitors off (the PC keeps running)
//   pc_sleep          action        put the PC to sleep (suspend)
//
// Another panel binds this device and invokes the actions; neither takes arguments. An action answers failure when no PC
// is linked. Success means the request was handed to the PC, not that the PC has acted on it yet.
//
// What this panel does with its own screen is the same whoever asked. Monitors off: nothing here; the PC reports its
// monitors off and the panel follows that report, as when the PC blanks them itself. Sleep: there is no report to
// follow, since the link goes down with the PC, so the panel blanks itself and its LED strip, and a touch brings those
// back. Nothing here can wake a sleeping PC.

/// Declare the resources. Once, before the network starts, with the other *_begin() calls.
void PcPower_begin();

/// Publish the link state when it changes and finish what an action started. LVGL task; cheap, call it from a timer.
void PcPower_tick();

/// Turn the PC's monitors off. False when no PC is linked. Any task.
bool PcPower_monitorsOff();

/// Put the PC to sleep and blank this panel. False when no PC is linked. Any task: the blanking happens on the next
/// PcPower_tick().
bool PcPower_sleep();
