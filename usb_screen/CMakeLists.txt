# USB extended display for Windows (see usb_screen.h and NOTES.md). Product-agnostic: no LVGL, no NightMare.
# The USB device itself (PHY, TinyUSB stack and configuration, connect/disconnect) is usb_device/; this component is
# its "screen" personality: tusb/usb_descriptors.c holds the descriptors it presents.
idf_component_register(
    SRCS usb_screen.c usb_screen_video.c usb_screen_touch.c tusb/usb_descriptors.c
    INCLUDE_DIRS "." "tusb"
    REQUIRES esp_lcd
    PRIV_REQUIRES usb usb_device esp_driver_jpeg esp_timer esp_hw_support
)
