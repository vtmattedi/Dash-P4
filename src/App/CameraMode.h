#pragma once

// The dashboard's side of the camera (builds with DASH_CAMERA): runs the board's MIPI-CSI camera when something wants
// the picture -- the Camera page's preview, or a PC using the panel as a webcam -- and stops it again when nothing
// does; keeps the picture adjustments (exposure, white balance, saturation, brightness, contrast) and saves them;
// and decides whether the USB-C port presents the PC screen or the camera (usb_device personalities: one OR the other).
//
// Everything here runs on the LVGL task unless said otherwise (call it with the LVGL lock held, or from LVGL
// callbacks); the camera runs on its own tasks and takes the lock itself when it hands a preview frame to LVGL.

#include <cstdint>

#include <NightMare.h>

#include "lvgl.h"

#include "camera.h"

/// The preview is this size: the sensor's 1288x728 at exactly half.
constexpr uint16_t kCameraPreviewW = 644;
constexpr uint16_t kCameraPreviewH = 364;

/// Call once, after App_loadSettings() (it reads the preferences) and before PcScreen_begin(): the USB port starts
/// with the personality the user left it in.
void CameraMode_begin();

/// A page that shows the camera: `image` is the lv_image the preview frames are set into; nullptr = nobody is looking.
/// While an image is set the camera runs.
void CameraMode_setPreview(lv_obj_t *image);

camera_adjust_t CameraMode_adjust();
/// Apply the adjustments now (live, while a slider is dragged); `persist` also saves them.
void CameraMode_setAdjust(const camera_adjust_t &adjust, bool persist);

/// The USB-C port presents the camera as a webcam (true) or the PC screen (false). Switching takes about half a
/// second and happens in the background: the PC sees one device go and another appear.
bool CameraMode_sharing();
void CameraMode_setSharing(bool on);

bool CameraMode_running();
float CameraMode_fps();
/// One line for the Camera page: what the USB port and the camera are doing. Valid until the next call.
const char *CameraMode_status();

/// Console:
///   CAMERA                         running, fps, USB state and the adjustments (JSON)
///   CAMERA SHOW | HOME             open the Camera tab / the Home tab
///   CAMERA SHARE ON|OFF            the USB-C port presents the webcam / the PC screen
///   CAMERA SET <ev|br|sat|con|warm> <value>   one adjustment, in the units of camera_adjust_t (live, saved)
///   CAMERA SET WB AUTO|HOLD        the white balance
///   CAMERA RESET                   every adjustment back to as-tuned
/// Any task; takes the LVGL lock.
NightMareResults CameraMode_command(const NightMareMessage &message);
