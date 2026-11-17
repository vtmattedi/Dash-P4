#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Called for every ESP-NOW frame the C6 reports, before the library's own receive callback sees it (on the ESP-Hosted
/// task: keep it short). `src` is the sender's MAC.
typedef void (*nm_espnow_tap_t)(const uint8_t src[6], int rssi, int channel, const uint8_t *payload, int len);

/// One observer (the ESP-NOW scanner); NULL removes it.
void nm_espnow_set_tap(nm_espnow_tap_t tap);

#ifdef __cplusplus
}
#endif
