/*----------------------------------------------------------
 * NightMareConfig.h -- feature selection for the NightMare Network library.
 *
 * Picked up by the library through __has_include(<NightMareConfig.h>); anything
 * left out takes the library's default. The whole set is listed on purpose: this
 * is the one place to read the panel's feature choices.
 *----------------------------------------------------------*/

#pragma once

#include <Version.h>

#ifdef VERSION
#define NM_FIRMWARE_VERSION VERSION
#endif

#pragma region "Services"

#define NM_ENABLE_SETTINGS 1
#define NM_ENABLE_RESOURCES 1
#define NM_ENABLE_NETWORK 1
#define NM_ENABLE_CONSOLE 1
#define NM_ENABLE_WIFI 1
// MQTT is the primary transport.
#define NM_ENABLE_MQTT 1
// ESP-NOW needs the ESP32-C6 to run the dash-p4-c6 firmware (ESP-Hosted slave + ESP-NOW bridge) and the
// espnow_hosted component on this side; only env jc1060p470c_usbscreen_nm_espnow builds with both
// (-DNM_NETWORK_ESPNOW=1). Every other env: MQTT only, remote broker (the user's call, D29).
#ifndef NM_NETWORK_ESPNOW
#define NM_NETWORK_ESPNOW 0
#endif
#define NM_ENABLE_TELEMETRY 1
#define NM_ENABLE_SCHEDULER 1
#define NM_ENABLE_JOBS 1
#define NM_ENABLE_TIME_SYNC 1
// The Explorer reads manifests; MessagePack is canonical, JSON is a convenience
// some devices also publish.
#define NM_ENABLE_JSON_WIRE 1

#define NM_WIFI_AUTO 1

// Diagnostic only, and costly: skip it.
#define NM_ENABLE_REMOTE_RESOURCE_VERIFICATION 0

// The serial console stays available independently of the network.
#define NM_CONSOLE_SERIAL 1
#define NM_CONSOLE_BUILTINS 1

// OTA in the library is ArduinoOTA-only. A native one is plan P8.
#define NM_ENABLE_OTA 0
#define NM_ENABLE_HTTP 0
#define NM_ENABLE_WEBSOCKET 0
// The panel drives LVGL itself (board/); the library's LVGL helpers would be a
// second owner of the display.
#define NM_ENABLE_LVGL 0

#pragma endregion

#pragma region "Threading"

// Scheduler jobs run from tickNightMareESP(), which the app calls from its own
// "nm" task -- never from the LVGL task, and never in a task of the library's.
// Jobs must only publish or post to the app's inbox; they must not touch UI state.
#define NM_SCHEDULER_OWN_TASK 0

#pragma endregion

#pragma region "Time"

// POSIX TZ for UTC-3 without daylight saving. Overridden at runtime by the
// `ac_timezone` setting.
#define NM_TIMEZONE "<-03>3"

#define NM_NTP_SERVER_1 "pool.ntp.org"
#define NM_NTP_SERVER_2 "time.nist.gov"
#define NM_NTP_SERVER_3 "time.google.com"

#pragma endregion

#pragma region "Bookkeeping"

#define NM_TELEMETRY_INTERVAL_MS 60000UL
#define NM_NETWORK_TELEMETRY_INTERVAL_MS 300000UL
#define NM_IDENTITY_CLEANUP_RETRY_MS 60000UL

#define NM_ENABLE_ACTION_PAYLOAD_ASSERTION 0

// NM_LOG_LEVEL_WARNING (2): the serial log is read while debugging the panel,
// and the library's INFO chatter would bury the app's own lines.
#define NM_LOG_LEVEL 2

#pragma endregion
