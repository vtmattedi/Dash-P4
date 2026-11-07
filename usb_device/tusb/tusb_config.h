/*
 * SPDX-FileCopyrightText: 2019 Ha Thach (tinyusb.org)
 * SPDX-FileContributor: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: MIT
 *
 * TinyUSB configuration for every USB personality of the P4's high-speed port: the Windows USB screen (vendor bulk
 * pipe for frames + HID digitizer for touch) and the USB webcam (UVC, MJPEG over a bulk endpoint). All the classes
 * are compiled in; which of them are active is decided by the configuration descriptor the selected personality
 * returns (see usb_device.h). TinyUSB finds this file through the include path usb_device/CMakeLists.txt adds to the
 * tinyusb component. Derived from esp-iot-solution examples/usb/device/usb_extend_screen/main/tusb/tusb_config.h.
 */

#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

// The P4's high-speed OTG controller (UTMI PHY, the board's USB-C "high-speed" port) is root hub port 1.
#define CFG_TUSB_RHPORT1_MODE (OPT_MODE_DEVICE | OPT_MODE_HIGH_SPEED)
#define CFG_TUD_MAX_SPEED OPT_MODE_HIGH_SPEED

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_FREERTOS
#endif

// Espressif IDF requires "freertos/" prefix in include path
#define CFG_TUSB_OS_INC_PATH freertos/

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 0
#endif

#define CFG_TUD_ENABLED 1

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE 64
#endif

//------------- CLASS -------------//
// Screen: vendor class, the frame stream from the Windows driver (bulk OUT).
#define CFG_TUD_VENDOR 1
#define VENDOR_BUF_SIZE 512
#define CFG_TUD_VENDOR_RX_BUFSIZE (VENDOR_BUF_SIZE * 10)
#define CFG_TUD_VENDOR_TX_BUFSIZE VENDOR_BUF_SIZE
#define CFG_TUD_VENDOR_EPSIZE VENDOR_BUF_SIZE

// Screen: HID, the touch digitizer. The configuration descriptor leaves the interface out when the screen is started
// without a touch controller (and the product id changes with it, see usb_screen/tusb/usb_descriptors.c).
#define CFG_TUD_HID 1
#define CFG_TUD_HID_EP_BUFSIZE 64

// Camera: one video control interface and one streaming interface.
#define CFG_TUD_VIDEO 1
#define CFG_TUD_VIDEO_STREAMING 1
// Bulk streaming: no alternate setting and no reserved bandwidth, and every host's UVC driver takes it. This is the
// size of one payload transfer (the stack's own buffer, in internal RAM).
#define CFG_TUD_VIDEO_STREAMING_BULK 1
#define CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE 4096

#ifdef __cplusplus
}
#endif
