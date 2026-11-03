#pragma once

// Stage-1 firmware's own screen: what the panel shows while the PC is not on it -- the connection state, what
// to do next, and the frame counters. Build once with the LVGL lock held, after PcLink_begin().
void StatusScreen_begin();
