/*
 * SPDX-FileCopyrightText: 2019 Ha Thach (tinyusb.org)
 * SPDX-FileContributor: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: MIT
 *
 * Derived from esp-iot-solution examples/usb/device/usb_extend_screen/main/tusb/usb_descriptors.c:
 * audio removed, the HID interface chosen at run time, the buffer limit sent in KB (the driver's unit).
 * The three descriptor functions are the screen personality (see usb_device.h): usb_device calls them while the screen is
 * what the USB port presents.
 */

#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "tusb.h"
#include "usb_descriptors.h"

// The Windows driver (Espressif's signed build of xfz1986_usb_graphic) binds to these ids: 0x2986 for the
// composite device (display + touch, the driver then takes interface MI_00), 0x2987 for a display alone.
#define USB_VID 0x303A
#define USB_PID_COMPOSITE 0x2986
#define USB_PID_DISPLAY_ONLY 0x2987

static bool s_touch;

static tusb_desc_device_t s_desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_UNSPECIFIED,
    .bDeviceSubClass = TUSB_CLASS_UNSPECIFIED,
    .bDeviceProtocol = TUSB_CLASS_UNSPECIFIED,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID_COMPOSITE,
    .bcdDevice = 0x0101,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

uint8_t const *usb_screen_desc_device(void)
{
    return (uint8_t const *)&s_desc_device;
}

//--------------------------------------------------------------------+
// HID report descriptor (the resolution is patched in by usb_descriptors_init)
//--------------------------------------------------------------------+
static uint8_t s_desc_hid_report[] = {
    TUD_HID_REPORT_DESC_TOUCH_SCREEN(REPORT_ID_TOUCH, 0, 0),
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_desc_hid_report;
}

enum {
    STR_INDEX_VENDOR = 4,
    STR_INDEX_HID,
};

//--------------------------------------------------------------------+
// Configuration descriptors: with and without the touch interface
//--------------------------------------------------------------------+
// The panel takes more than USB 2.0's 500 mA at full brightness; asking for the maximum is the honest number.
#define USB_SCREEN_MAX_POWER_MA 500
#define CONFIG_LEN_DISPLAY (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN)
#define CONFIG_LEN_TOUCH (CONFIG_LEN_DISPLAY + TUD_HID_DESC_LEN)

static uint8_t const s_desc_config_display[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_LEN_DISPLAY, 0, USB_SCREEN_MAX_POWER_MA),
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, STR_INDEX_VENDOR, EPNUM_VENDOR, 0x80 | EPNUM_VENDOR, CFG_TUD_VENDOR_EPSIZE),
};

static uint8_t const s_desc_config_touch[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CONFIG_LEN_TOUCH, 0, USB_SCREEN_MAX_POWER_MA),
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, STR_INDEX_VENDOR, EPNUM_VENDOR, 0x80 | EPNUM_VENDOR, CFG_TUD_VENDOR_EPSIZE),
    // Interface number, string index, protocol, report descriptor len, EP In address, size & polling interval (2^(4-1) microframes = 1 ms)
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, STR_INDEX_HID, HID_ITF_PROTOCOL_NONE, sizeof(s_desc_hid_report),
                       (0x80 | EPNUM_HID_DATA), CFG_TUD_HID_EP_BUFSIZE, 4),
};

uint8_t const *usb_screen_desc_configuration(uint8_t index)
{
    (void)index;
    return s_touch ? s_desc_config_touch : s_desc_config_display;
}

//--------------------------------------------------------------------+
// String descriptors
//--------------------------------------------------------------------+
// The vendor interface string is how the driver learns the panel: <chip>udisp<n>_R<w>x<h>_Ejpg<q>_Fps<f>_Bl<KB>.
static char s_vendor_str[64];
static char s_serial_str[24];

static char const *s_string_desc[] = {
    (const char[]){0x09, 0x04}, // 0: English (0x0409)
    "Espressif",                // 1: Manufacturer (the driver's INF does not care; kept as in the reference)
    "ESP_Extern_Screen",        // 2: Product
    s_serial_str,               // 3: Serial
    s_vendor_str,               // 4: Vendor interface
    "touch",                    // 5: HID interface
};

static uint16_t s_desc_str[64];

esp_err_t usb_descriptors_init(const usb_descriptors_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->width && cfg->height, ESP_ERR_INVALID_ARG, "usb_desc", "invalid display resolution");
    s_touch = cfg->touch;
    s_desc_device.idProduct = cfg->touch ? USB_PID_COMPOSITE : USB_PID_DISPLAY_ONLY;

    // Logical/physical maxima alternate X, Y in each of the five finger collections.
    unsigned logical_max_count = 0;
    unsigned physical_max_count = 0;
    for (size_t i = 0; i + 2 < sizeof(s_desc_hid_report); i++) {
        uint16_t value;
        if (s_desc_hid_report[i] == 0x26) {
            value = (logical_max_count++ % 2 == 0) ? cfg->width : cfg->height;
        } else if (s_desc_hid_report[i] == 0x46) {
            value = (physical_max_count++ % 2 == 0) ? cfg->width : cfg->height;
        } else {
            continue;
        }
        s_desc_hid_report[i + 1] = value & 0xff;
        s_desc_hid_report[i + 2] = value >> 8;
        i += 2;
    }
    ESP_RETURN_ON_FALSE(logical_max_count == 10 && physical_max_count == 10, ESP_ERR_INVALID_STATE, "usb_desc",
                        "unexpected HID report layout");

    // Bl is in KB: the driver multiplies it by 1024, and lowers the JPEG quality of a frame that would exceed it.
    int written = snprintf(s_vendor_str, sizeof(s_vendor_str), CONFIG_IDF_TARGET "udisp0_R%ux%u_Ejpg%u_Fps%u_Bl%u",
                           cfg->width, cfg->height, cfg->jpeg_quality, cfg->max_fps,
                           (unsigned)(cfg->frame_limit / 1024));
    ESP_RETURN_ON_FALSE(written > 0 && written < (int)sizeof(s_vendor_str), ESP_ERR_INVALID_SIZE, "usb_desc",
                        "vendor descriptor string is too long");
    snprintf(s_serial_str, sizeof(s_serial_str), "%s", cfg->serial ? cfg->serial : "000000000000");
    return ESP_OK;
}

uint16_t const *usb_screen_desc_string(uint8_t index, uint16_t langid)
{
    (void)langid;
    uint8_t chr_count;

    if (index == 0) {
        memcpy(&s_desc_str[1], s_string_desc[0], 2);
        chr_count = 1;
    } else {
        // 0xEE (Microsoft OS 1.0 descriptor) and anything else unknown: not supported.
        if (!(index < sizeof(s_string_desc) / sizeof(s_string_desc[0]))) {
            return NULL;
        }
        const char *str = s_string_desc[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 63) {
            chr_count = 63;
        }
        for (uint8_t i = 0; i < chr_count; i++) {
            s_desc_str[1 + i] = str[i];
        }
    }
    s_desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return s_desc_str;
}
