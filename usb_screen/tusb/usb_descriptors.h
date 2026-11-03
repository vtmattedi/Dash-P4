/*
 * SPDX-FileCopyrightText: 2020 Jerzy Kasenbreg
 * SPDX-FileCopyrightText: 2021 Koji KITAYAMA
 * SPDX-FileContributor: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: MIT
 *
 * Derived from esp-iot-solution examples/usb/device/usb_extend_screen/main/tusb/usb_descriptors.h
 * (audio removed, HID chosen at run time).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "tusb.h"

typedef struct {
    uint16_t width;
    uint16_t height;
    bool touch;           // HID digitizer interface present (composite device, PID 0x2986)
    uint8_t jpeg_quality; // 1..10
    uint8_t max_fps;
    uint32_t frame_limit; // bytes
    const char *serial;
} usb_descriptors_config_t;

/// Fill in the run-time parts (resolution in the HID report and the vendor string). Call before tusb_init().
esp_err_t usb_descriptors_init(const usb_descriptors_config_t *cfg);

enum {
    REPORT_ID_TOUCH = 1,
    REPORT_ID_MAX_COUNT,
    REPORT_ID_COUNT
};

enum {
    ITF_NUM_VENDOR = 0,
    ITF_NUM_HID,
};

enum {
    EPNUM_DEFAULT = 0,
    EPNUM_VENDOR,
    EPNUM_HID_DATA,
};

// Five-finger touch screen (Windows multi-touch digitizer, hybrid mode: one report carries every contact).
#define TUD_HID_REPORT_DESC_TOUCH_SCREEN(report_id, width, height)                                                    \
    HID_USAGE_PAGE(HID_USAGE_PAGE_DIGITIZER),                                                                        \
    /* USAGE (Touch Screen) */                                                                                       \
    HID_USAGE(0x04),                                                                                                 \
    HID_COLLECTION(HID_COLLECTION_APPLICATION),                                                                      \
    HID_REPORT_ID(report_id)                                                                                         \
    FINGER_USAGE(width, height)                                                                                      \
    FINGER_USAGE(width, height)                                                                                      \
    FINGER_USAGE(width, height)                                                                                      \
    FINGER_USAGE(width, height)                                                                                      \
    FINGER_USAGE(width, height)                                                                                      \
    /* Contact count */                                                                                              \
    HID_USAGE(0x54),                                                                                                 \
    HID_LOGICAL_MAX(127),                                                                                            \
    HID_REPORT_COUNT(1),                                                                                             \
    HID_REPORT_SIZE(8),                                                                                              \
    HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),                                                               \
    HID_REPORT_ID(report_id + 1)                                                                                     \
    /* Contact count maximum (feature) */                                                                            \
    HID_USAGE(0x55),                                                                                                 \
    HID_REPORT_COUNT(1),                                                                                             \
    HID_LOGICAL_MAX(0x10),                                                                                           \
    HID_FEATURE(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),                                                             \
    HID_COLLECTION_END

#define FINGER_USAGE(width, height)                                                                                  \
    HID_USAGE(0x42),                                                                                                 \
    HID_COLLECTION(HID_COLLECTION_LOGICAL),                                                                          \
    /* Tip switch, 1 bit + 7 bits padding */                                                                         \
    HID_USAGE(0x42),                                                                                                 \
    HID_LOGICAL_MIN(0x00),                                                                                           \
    HID_LOGICAL_MAX(0x01),                                                                                           \
    HID_REPORT_SIZE(1),                                                                                              \
    HID_REPORT_COUNT(1),                                                                                             \
    HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),                                                               \
    HID_REPORT_COUNT(7),                                                                                             \
    HID_INPUT(HID_CONSTANT | HID_ARRAY | HID_ABSOLUTE),                                                              \
    /* Contact identifier */                                                                                         \
    HID_REPORT_SIZE(8),                                                                                              \
    HID_USAGE(0x51),                                                                                                 \
    HID_REPORT_COUNT(1),                                                                                             \
    HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),                                                               \
    HID_USAGE_PAGE(HID_USAGE_PAGE_DESKTOP),                                                                          \
    HID_LOGICAL_MAX_N(width, 2),                                                                                     \
    HID_REPORT_SIZE(16),                                                                                             \
    HID_UNIT_EXPONENT(0x0e),                                                                                         \
    /* Inch, EngLinear */                                                                                            \
    HID_UNIT(0x13),                                                                                                  \
    /* X */                                                                                                          \
    HID_USAGE(0x30),                                                                                                 \
    HID_PHYSICAL_MIN(0),                                                                                             \
    HID_PHYSICAL_MAX_N(width, 2),                                                                                    \
    HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),                                                               \
    /* Y */                                                                                                          \
    HID_LOGICAL_MAX_N(height, 2),                                                                                    \
    HID_PHYSICAL_MAX_N(height, 2),                                                                                   \
    HID_USAGE(0x31),                                                                                                 \
    HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),                                                               \
    HID_USAGE_PAGE(HID_USAGE_PAGE_DIGITIZER),                                                                        \
    /* Width, Height */                                                                                              \
    HID_USAGE(0x48),                                                                                                 \
    HID_USAGE(0x49),                                                                                                 \
    HID_REPORT_COUNT(2),                                                                                             \
    HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),                                                               \
    HID_COLLECTION_END,
