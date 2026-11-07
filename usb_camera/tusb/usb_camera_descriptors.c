/*
 * SPDX-FileCopyrightText: 2019 Ha Thach (tinyusb.org)
 * SPDX-FileCopyrightText: 2020 Jerzy Kasenbreg
 * SPDX-FileCopyrightText: 2021 Koji KITAYAMA
 *
 * SPDX-License-Identifier: MIT
 *
 * Descriptors of the USB camera: one UVC 1.5 function, MJPEG, a single frame size, bulk streaming endpoint.
 * The descriptor macros are the ones in TinyUSB's examples/device/video_capture (usb_descriptors.h, kept there
 * "as reference"). The three descriptor functions are the camera personality (see usb_device.h): usb_device calls them
 * while the camera is what the USB port presents.
 */

#include <stdio.h>
#include <string.h>
#include "esp_mac.h"
#include "tusb.h"
#include "usb_camera.h"
#include "usb_camera_descriptors.h"

// Not an Espressif-assigned product id: the display firmware uses 0x2986/0x2987 and the Windows driver for it
// binds to those, so the camera must use a different one (the PC remembers drivers per VID/PID).
#define USB_VID 0x303A
#define USB_PID 0x2988
#define USB_BCD 0x0200

// Time stamp base clock. A deprecated parameter.
#define UVC_CLOCK_FREQUENCY 27000000

#define UVC_ENTITY_CAP_INPUT_TERMINAL 0x01
#define UVC_ENTITY_CAP_OUTPUT_TERMINAL 0x02

#define EPNUM_VIDEO_IN 0x81
// High-speed bulk endpoint packet size.
#define VIDEO_EP_SIZE 512

enum {
    ITF_NUM_VIDEO_CONTROL,
    ITF_NUM_VIDEO_STREAMING,
    ITF_NUM_TOTAL
};

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_UVC_CONTROL,
    STRID_UVC_STREAMING,
};

// Lengths of the pieces of the MJPEG-over-bulk function (TinyUSB example, TUD_VIDEO_CAPTURE_DESC_MJPEG_BULK_LEN).
#define DESC_VIDEO_LEN (                                                                                       \
    TUD_VIDEO_DESC_IAD_LEN                                                                                      \
    /* control */                                                                                               \
    + TUD_VIDEO_DESC_STD_VC_LEN + (TUD_VIDEO_DESC_CS_VC_LEN + 1) + TUD_VIDEO_DESC_CAMERA_TERM_LEN               \
    + TUD_VIDEO_DESC_OUTPUT_TERM_LEN                                                                            \
    /* streaming, alternate 0 (the only one: bulk) */                                                           \
    + TUD_VIDEO_DESC_STD_VS_LEN + (TUD_VIDEO_DESC_CS_VS_IN_LEN + 1) + TUD_VIDEO_DESC_CS_VS_FMT_MJPEG_LEN        \
    + TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT_LEN + TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN + 7 /* endpoint */)

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + DESC_VIDEO_LEN)

#define W USB_CAMERA_WIDTH
#define H USB_CAMERA_HEIGHT
#define FPS USB_CAMERA_FPS

static uint8_t const s_desc_configuration[] = {
    // Configuration: number, interfaces, string index, total length, attributes (bus powered), power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 500),

    TUD_VIDEO_DESC_IAD(ITF_NUM_VIDEO_CONTROL, /* 2 interfaces */ 0x02, STRID_UVC_CONTROL),
    /* Video control */
    TUD_VIDEO_DESC_STD_VC(ITF_NUM_VIDEO_CONTROL, 0, STRID_UVC_CONTROL),
        /* Header: UVC 1.5, length of the descriptors that follow, clock (deprecated), streaming interface */
        TUD_VIDEO_DESC_CS_VC(0x0150, TUD_VIDEO_DESC_CAMERA_TERM_LEN + TUD_VIDEO_DESC_OUTPUT_TERM_LEN,
                             UVC_CLOCK_FREQUENCY, ITF_NUM_VIDEO_STREAMING),
            /* Camera terminal: id, bAssocTerminal, iTerminal, focal min, max, length, bmControls */
            TUD_VIDEO_DESC_CAMERA_TERM(UVC_ENTITY_CAP_INPUT_TERMINAL, 0, 0, 0, 0, 0, 0),
            TUD_VIDEO_DESC_OUTPUT_TERM(UVC_ENTITY_CAP_OUTPUT_TERMINAL, VIDEO_TT_STREAMING, 0,
                                       UVC_ENTITY_CAP_INPUT_TERMINAL, 0),
    /* Video streaming, alternate 0: one bulk endpoint */
    TUD_VIDEO_DESC_STD_VS(ITF_NUM_VIDEO_STREAMING, 0, 1, STRID_UVC_STREAMING),
        /* Input header, no still image capture */
        TUD_VIDEO_DESC_CS_VS_INPUT(/*bNumFormats*/ 1,
            /*wTotalLength - bLength*/ TUD_VIDEO_DESC_CS_VS_FMT_MJPEG_LEN + TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT_LEN +
                TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN,
            EPNUM_VIDEO_IN, /*bmInfo*/ 0, /*bTerminalLink*/ UVC_ENTITY_CAP_OUTPUT_TERMINAL,
            /*bStillCaptureMethod*/ 0, /*bTriggerSupport*/ 0, /*bTriggerUsage*/ 0, /*bmaControls(1)*/ 0),
            TUD_VIDEO_DESC_CS_VS_FMT_MJPEG(/*bFormatIndex*/ 1, /*bNumFrameDescriptors*/ 1, /*bmFlags*/ 0,
                                           /*bDefaultFrameIndex*/ 1, 0, 0, 0, /*bCopyProtect*/ 0),
                TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT(/*bFrameIndex*/ 1, 0, W, H, W * H * 16, W * H * 16 * FPS,
                                                    W * H * 16 / 8, (10000000 / FPS), (10000000 / FPS),
                                                    (10000000 / FPS) * FPS, (10000000 / FPS)),
            TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING(VIDEO_COLOR_PRIMARIES_BT709, VIDEO_COLOR_XFER_CH_BT709,
                                                VIDEO_COLOR_COEF_SMPTE170M),
            TUD_VIDEO_DESC_EP_BULK(EPNUM_VIDEO_IN, VIDEO_EP_SIZE, 1),
};

uint8_t const *usb_camera_desc_configuration(uint8_t index)
{
    (void)index;
    return s_desc_configuration;
}

static tusb_desc_device_t const s_desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = USB_BCD,
    // A function described by an Interface Association Descriptor: the device class is "miscellaneous / common / IAD".
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 0x01,
};

uint8_t const *usb_camera_desc_device(void)
{
    return (uint8_t const *)&s_desc_device;
}

static char s_serial[13];

static char const *const s_string_desc[] = {
    [STRID_LANGID] = (const char[]){0x09, 0x04}, // English (0x0409)
    [STRID_MANUFACTURER] = "NightMare",
    [STRID_PRODUCT] = "NightMare Dash Camera",
    [STRID_SERIAL] = s_serial,
    [STRID_UVC_CONTROL] = "Dash Camera Control",
    [STRID_UVC_STREAMING] = "Dash Camera Streaming",
};

static uint16_t s_desc_str[32 + 1];

uint16_t const *usb_camera_desc_string(uint8_t index, uint16_t langid)
{
    (void)langid;
    if (!s_serial[0]) {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_BASE);
        snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
                 mac[5]);
    }

    uint8_t chr_count;
    if (index == STRID_LANGID) {
        memcpy(&s_desc_str[1], s_string_desc[0], 2);
        chr_count = 1;
    } else {
        // 0xEE (Microsoft OS 1.0 descriptor) and anything else unknown: not supported.
        if (index >= sizeof(s_string_desc) / sizeof(s_string_desc[0])) {
            return NULL;
        }
        const char *str = s_string_desc[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) {
            chr_count = 31;
        }
        for (uint8_t i = 0; i < chr_count; i++) {
            s_desc_str[1 + i] = str[i];
        }
    }
    s_desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return s_desc_str;
}
