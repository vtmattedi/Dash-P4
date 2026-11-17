#pragma once
#include <stdint.h>
#include <string>

// Over-the-air firmware update, native ESP-IDF (the NightMare library's own OTA is
// ArduinoOTA and does not exist in this build).
//
// The partition table has two 6 MB app slots. An update streams a firmware.bin from an
// http:// or https:// URL into the idle slot with esp_https_ota, then the caller restarts.
// With app rollback enabled, a new image that does not call Ota_markValid() (the app does so
// 20 s after a healthy start) is rolled back by the bootloader on the next reset.
//
// Typical LAN use: `python -m http.server` in `.pio/build/<env>/` and the URL of firmware.bin.

enum class OtaState : uint8_t
{
    Idle,
    Running,
    Done,   // written and verified; restart to boot it
    Failed
};

/// Start an update on its own task. False if one is already running or the URL is empty.
bool Ota_start(const std::string &url);

/// Update the ESP32-C6 co-processor instead: streams an ESP32-C6 application image (an ESP-Hosted slave, e.g. the
/// dash-p4-c6 firmware.bin) through ESP-Hosted's OTA calls into the C6's idle slot. Refuses an image built for
/// another chip. Same state, percent and message as a P4 update; when Done, Ota_restartAfterCoprocessor() boots it.
bool Ota_startCoprocessor(const std::string &url);
/// After a Done co-processor update: tell the C6 to switch to the new image (slaves 2.6 and later need that;
/// older ones switched at the end of the write), then restart the P4, which also resets the C6.
void Ota_restartAfterCoprocessor();
/// The update that ran last (or runs now) was for the co-processor.
bool Ota_isCoprocessor();

// --- Push: the image arrives in pieces over the PC link (dashlink push-fw) instead of being pulled from a URL.
// Begin, then Write for every piece in order, and End when the last one is in. End checks the size and the sha256 the PC
// announced, validates the image and switches the boot slot. Any failure aborts and leaves the running image untouched.
// All calls come from one task (the link task).

/// Start a push. `coprocessor`: the image is for the ESP32-C6. `sha256Hex` is the announced hash of the whole image.
bool Ota_pushBegin(bool coprocessor, uint32_t size, const std::string &sha256Hex, std::string &why);
/// Append the next piece. False (and the push is aborted, `why` says why) on any error.
bool Ota_pushWrite(const uint8_t *data, size_t length, std::string &why);
/// The announced size has arrived: verify and finish. True means Done.
bool Ota_pushEnd(std::string &why);
/// Drop a push in progress (the link went away): the half-written slot is never booted.
void Ota_pushAbort(const char *why);
bool Ota_pushActive();
/// Bytes received so far / announced, for a progress report.
uint32_t Ota_pushReceived();
uint32_t Ota_pushSize();
/// After a pushed update is Done: restart into it (the C6 is told to switch first for a C6 image).
void Ota_pushRestart();
/// sha256 of the ELF the running firmware was built from, 64 hex characters; the PC compares it to its build.
std::string Ota_runningBuildId();

OtaState Ota_state();
/// 0..100, or -1 while the image size is not known yet.
int Ota_percent();
/// Last status or error text.
std::string Ota_message();

/// Tell the bootloader the running image is good (cancels a pending rollback).
void Ota_markValid();
