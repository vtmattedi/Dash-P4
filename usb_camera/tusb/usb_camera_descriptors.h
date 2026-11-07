#pragma once

// The camera personality's descriptors (see usb_device.h). Registered with usb_device by usb_camera_start().

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint8_t const *usb_camera_desc_device(void);
uint8_t const *usb_camera_desc_configuration(uint8_t index);
uint16_t const *usb_camera_desc_string(uint8_t index, uint16_t langid);

#ifdef __cplusplus
}
#endif
