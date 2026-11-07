#pragma once

// The USB device on the P4's high-speed port, shared by everything that can be plugged into a PC through it.
//
// This component owns what there is only one of: the USB PHY, the TinyUSB stack and its configuration (tusb/), the
// device task, and the connect/disconnect of the bus. What the PC sees -- the device, configuration and string
// descriptors -- belongs to a "personality" that a component registers: the Windows USB screen (usb_screen/) or the
// USB webcam (usb_camera/). One personality is presented at a time. Selecting another one is a soft disconnect, the
// new descriptor set, and a soft reconnect: the PC sees a different device appear (with a different VID/PID, so each
// keeps its own driver).
//
// Product-agnostic: no LVGL, no NightMare.

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    USB_DEVICE_NONE = 0, ///< off the bus: the PC sees nothing
    USB_DEVICE_SCREEN,   ///< Windows extended display + touch (usb_screen/)
    USB_DEVICE_CAMERA,   ///< UVC webcam (usb_camera/)
    USB_DEVICE_KIND_COUNT
} usb_device_kind_t;

/// What a personality provides. Every hook is optional except the three descriptor callbacks. The descriptor
/// callbacks have the signature (and the lifetime rules) of TinyUSB's tud_descriptor_*_cb; the other hooks run on the
/// USB task, or on the task that called usb_device_select() for on_unmount, and must be quick.
typedef struct {
    const char *name;
    const uint8_t *(*device_descriptor)(void);
    const uint8_t *(*configuration_descriptor)(uint8_t index);
    const uint16_t *(*string_descriptor)(uint8_t index, uint16_t langid);
    void (*on_mount)(void);   ///< the host configured the device
    void (*on_unmount)(void); ///< bus reset, cable out, or this personality was deselected
    void (*on_suspend)(bool remote_wakeup_enabled);
    void (*on_resume)(void);
} usb_device_personality_t;

/// Make a personality available. Call before usb_device_start() or at any time before selecting it. Not copied:
/// the table must outlive the program.
esp_err_t usb_device_register(usb_device_kind_t kind, const usb_device_personality_t *personality);

/// Say which personality the firmware wants on the bus from the start (for example the one the user chose last time),
/// whichever component happens to register or start first. Call it before the components start. usb_device_start()
/// then presents the preferred personality instead of its argument; while the preferred one is not registered yet the
/// bus stays off, and it is selected the moment it registers. Without a preference, usb_device_start() decides.
void usb_device_prefer(usb_device_kind_t kind);

/// Start the PHY, the stack and its task, presenting `initial` on the bus (USB_DEVICE_NONE: start off the bus). A
/// second call only returns ESP_OK, so a component may call it as a convenience: the first caller's choice wins.
esp_err_t usb_device_start(usb_device_kind_t initial);
bool usb_device_started(void);

/// Present another personality (or none). A no-op if it is already the one. When the bus was up this disconnects,
/// waits for the PC to notice (about 400 ms), swaps the descriptors and reconnects, so it blocks for that long:
/// call it from a task that can wait, not from the LVGL task. Safe from any task.
esp_err_t usb_device_select(usb_device_kind_t kind);
usb_device_kind_t usb_device_current(void);
const char *usb_device_kind_name(usb_device_kind_t kind);

#ifdef __cplusplus
}
#endif
