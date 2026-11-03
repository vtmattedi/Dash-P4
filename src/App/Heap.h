#pragma once
#include <stddef.h>
#include <stdint.h>

// Memory readouts. The board has two pools that matter and they are not
// interchangeable: internal RAM (stacks, DMA, the Wi-Fi/hosted transport) and the
// 32MB PSRAM (LVGL, frame buffers, the app's tables). A healthy total can hide an
// exhausted internal pool, so both are always reported.

size_t Heap_internalFree();
size_t Heap_internalLargest();
size_t Heap_internalMinFree();
size_t Heap_psramFree();
size_t Heap_psramTotal();

/// One line on the serial log: `[heap] <tag>: internal=…K (largest …K, min …K) psram=…K`.
void Heap_log(const char *tag);
