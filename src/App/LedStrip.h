#pragma once
#include <stdint.h>

// The WS2812B strip on BOARD_LED_STRIP_GPIO (board/board_pins.h), driven through the RMT peripheral.
//
// Every pixel shows the same colour. `brightness` (0..255) scales it, so the stored colour stays the one that was asked for.
// Safe to call from any task: the strip is guarded by a mutex. Does nothing, and reports false, when the strip could not
// be set up (no RMT channel free, the component missing in a host build).

/// Create the RMT device and blank the strip. Call once at start-up.
void LedStrip_begin();

bool LedStrip_ready();

/// Show `rgb` (0xRRGGBB) on every pixel. Returns false when the strip is not ready.
bool LedStrip_setColour(uint32_t rgb);

/// Applied to whatever colour is showing and to later colours. Capped at BOARD_LED_STRIP_MAX_BRIGHTNESS (64): a higher
/// value is clamped to it, to keep the current draw down.
bool LedStrip_setBrightness(uint8_t level);

/// One colour per pixel (0xRRGGBB), for animations; `count` entries, missing ones are dark. Scaled by the brightness like a colour.
bool LedStrip_setFrame(const uint32_t *rgb, int count);
int LedStrip_count();

bool LedStrip_off();

/// Resend the current frame about once a second. A WS2812B that lost power for a moment (a sagging rail, a
/// supply that came up after the MCU) comes back black and stays so until it is written again. Cheap to call from a timer.
void LedStrip_tick();

/// Dark while true, whatever colour is set; the colour comes back when it is false again. For the PC's monitor going to
/// sleep: it does not change what LedStrip_off() / LedStrip_setColour() remember.
bool LedStrip_sleep(bool sleeping);

bool LedStrip_isOff();
uint32_t LedStrip_colour();
uint8_t LedStrip_brightness();
