#pragma once

// MIPI-CSI camera capture for the Guition JC1060P470C (OV02C10), through Espressif's esp_video (V4L2 on the
// ISP). Product-agnostic: no LVGL, no USB, no NightMare. The caller owns what happens to a frame.
//
// Frames are RGB565, the full sensor size (1920x1080 on this board), and live in PSRAM. The sensor module has
// one data lane; its control bus (SCCB) is the board's shared I2C bus on GPIO7/8, so the caller passes that bus
// in and the camera never creates its own.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *data; ///< packed RGB565, width * height * 2 bytes; valid only inside the callback
    size_t len;
    uint16_t width;
    uint16_t height;
    uint8_t index; ///< which capture buffer this is (0 .. buffer_count-1)
} camera_frame_t;

/// Called from the capture task for every frame. The buffer goes back to the driver when the callback returns,
/// so copy or consume it here. A slow callback lowers the frame rate (the driver drops frames meanwhile).
typedef void (*camera_frame_cb_t)(const camera_frame_t *frame, void *ctx);

typedef struct {
    i2c_master_bus_handle_t i2c_bus; ///< the board's shared bus (board_i2c_bus()); required
    int reset_gpio;                  ///< sensor reset, -1 if not wired (it is not, on this board)
    int pwdn_gpio;                   ///< sensor power-down, -1 if not wired
    uint8_t buffer_count;            ///< capture buffers, 2 or 3 (0 = 2)
    int8_t task_core;                ///< core the capture task is pinned to (-1 = any)
} camera_config_t;

/// What the user can ask of the picture, in plain units. The camera's auto-tuning loop keeps running underneath
/// (auto exposure, auto white balance, tuned colour); these are applied on top of what it computes, every frame
/// (see third_party/NOTES.md, the esp_video patch). The defaults change nothing.
typedef struct {
    int8_t exposure;    ///< exposure compensation, tenths of a stop (EV): -30..30. The auto-exposure loop settles brighter
                        ///< (+) or darker (-), within its exposure and gain limits.
    int8_t brightness;  ///< -50..50: a brightness offset added after the exposure is measured (lifts or lowers everything)
    uint8_t saturation; ///< percent of the tuned saturation: 0 (grey) .. 200
    uint8_t contrast;   ///< percent of the tuned contrast: 50 .. 150
    bool wb_hold;       ///< hold the white balance at the gains it had when this was switched on (auto stops adapting)
    int8_t warmth;      ///< -50..50, with wb_hold: + warmer (more red, less blue), - cooler
} camera_adjust_t;

#define CAMERA_ADJUST_DEFAULT() { .exposure = 0, .brightness = 0, .saturation = 100, .contrast = 100, .wb_hold = false, .warmth = 0 }

#define CAMERA_CONFIG_DEFAULT() \
    { .i2c_bus = NULL, .reset_gpio = -1, .pwdn_gpio = -1, .buffer_count = 2, .task_core = 0 }

/// Bring up the sensor and the ISP, allocate the buffers and start streaming into `cb`. Idempotent state:
/// returns ESP_ERR_INVALID_STATE while already running. The MIPI PHY supply (the on-chip LDO the display also
/// uses) must be on: board_init() / board_power_dsi_phy() does that.
esp_err_t camera_start(const camera_config_t *cfg, camera_frame_cb_t cb, void *ctx);

/// Stop streaming and release the buffers. Returns after the capture task has gone.
esp_err_t camera_stop(void);

/// Apply picture adjustments. Out-of-range values are clamped. Safe from any task, before camera_start() too (they take
/// effect when the camera runs), and they stay in effect across camera_stop() / camera_start().
esp_err_t camera_set_adjust(const camera_adjust_t *adjust);
/// The adjustments in effect.
void camera_get_adjust(camera_adjust_t *out);
/// The white-balance gains in effect now (automatic or held), for showing where the auto white balance is.
void camera_get_wb_gains(float *red, float *blue);

bool camera_running(void);
/// Frame size once started (0 before).
uint16_t camera_width(void);
uint16_t camera_height(void);
/// Frames delivered since camera_start().
uint32_t camera_frames(void);

#ifdef __cplusplus
}
#endif
