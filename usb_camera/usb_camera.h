#pragma once

// The panel as a USB webcam (UVC, MJPEG) on the P4's high-speed USB port. Windows, macOS and Linux use their own
// UVC driver: nothing to install on the PC.
//
// Product-agnostic: no LVGL, no NightMare, and it does not talk to the camera itself. The caller hands over
// RGB565 frames (camera/ produces them); this component JPEG-encodes them with the P4's hardware encoder and
// sends them to the host while the host has the stream open.
//
// It owns the TinyUSB device stack of its firmware (tusb/ holds the configuration and the descriptors), so it
// cannot be linked together with usb_screen/ yet: each firmware has one USB personality, screen OR camera.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// The one frame size the device offers. It is a compile-time constant because the UVC descriptors are, and it
// is the size of the camera's sensor mode (camera/, CONFIG_CAMERA_OV02C10_MIPI_RAW10_1288X728_30FPS).
#define USB_CAMERA_WIDTH 1288
#define USB_CAMERA_HEIGHT 728
#define USB_CAMERA_FPS 30 // advertised; the real rate follows the camera

typedef struct {
    uint8_t jpeg_quality; ///< 1..100 (the hardware encoder's scale), 0 = 80
    const char *serial;   ///< USB serial string; NULL: the MAC address
} usb_camera_config_t;

typedef struct {
    uint32_t sent;          ///< frames handed to the USB stack
    uint32_t dropped_busy;  ///< frames dropped because the previous two had not left yet
    uint32_t dropped_idle;  ///< frames dropped because the host has the stream closed
    uint32_t errors;        ///< encode or transfer failures
    uint32_t last_jpeg;     ///< size in bytes of the last encoded frame
    uint32_t encode_us_avg; ///< average encode time of the last frames
} usb_camera_stats_t;

/// Start the USB device on the high-speed port and the JPEG encoder. Call once.
esp_err_t usb_camera_start(const usb_camera_config_t *cfg);

/// True while the host has the video stream open (an application is using the camera). Frames offered while it
/// is false are dropped at once, without encoding.
bool usb_camera_streaming(void);
/// True once the host has enumerated the device (and not since gone away).
bool usb_camera_mounted(void);

/// Offer a frame: packed RGB565, USB_CAMERA_WIDTH x USB_CAMERA_HEIGHT. Encodes it and queues it for the host, or
/// drops it (not streaming, or two frames are already waiting). Blocks for the encode (a few ms). Returns
/// ESP_OK when the frame was queued, ESP_ERR_INVALID_STATE when it was dropped because nobody is watching,
/// ESP_ERR_NOT_FINISHED when it was dropped because the host is slower than the camera.
esp_err_t usb_camera_submit(const void *rgb565, size_t len, uint16_t width, uint16_t height);

void usb_camera_get_stats(usb_camera_stats_t *out);

#ifdef __cplusplus
}
#endif
