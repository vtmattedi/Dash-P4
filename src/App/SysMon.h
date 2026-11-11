#pragma once

// A tiny memory monitor for the top bar: how much of the internal RAM (the scarce pool: stacks, DMA, the Wi-Fi
// transport) and of the PSRAM is in use, in percent. Sampled about every two seconds.

struct SysMonState
{
    int internalUsedPct = -1; // 0..100; -1 before the first sample
    int psramUsedPct = -1;
    unsigned internalFreeKb = 0; // for the serial log and tooltips
    unsigned psramFreeKb = 0;
};

/// Pure: percent used from a free and a total size (same unit). Clamped to 0..100; -1 when the total is 0.
int SysMon_usedPercent(unsigned long long freeSize, unsigned long long totalSize);

/// Take a sample if the last one is older than about two seconds. Cheap to call from a timer (LVGL task).
void SysMon_sample();

SysMonState SysMon_state();
