#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Recording from the board's microphone (the MEMS microphone MIC1 behind the ES8311 codec). Record only: no speaker is
/// fitted, so the output side and the amplifier (GPIO11) are never switched on. Mono, 16-bit, BOARD_AUDIO_RATE.
#define BOARD_AUDIO_RATE 16000

/// Bring up the I2S receive channel and the codec. Safe to call again. Not done at boot: the codec and I2S use internal
/// RAM, so open it when a recording starts and close it after.
esp_err_t board_audio_open(void);
void board_audio_close(void);
bool board_audio_is_open(void);

/// Microphone gain in dB (0..37.5 on the ES8311). Applies at once when open, and to the next open otherwise.
void board_audio_set_gain(float db);

/// Read up to `samples` 16-bit samples, waiting as long as it takes (about samples / 16 ms). Returns how many were read, or
/// a negative esp_err_t.
int board_audio_read(int16_t *dst, size_t samples);

#ifdef __cplusplus
}
#endif
