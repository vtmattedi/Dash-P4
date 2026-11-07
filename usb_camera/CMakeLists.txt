# The panel as a USB webcam (see usb_camera.h). Product-agnostic: no LVGL, no NightMare.
# The USB device itself (PHY, TinyUSB stack and configuration, connect/disconnect) is usb_device/; this component is
# its "camera" personality: tusb/usb_camera_descriptors.c holds the descriptors it presents.
idf_component_register(
    SRCS usb_camera.c tusb/usb_camera_descriptors.c
    INCLUDE_DIRS "." "tusb"
    REQUIRES esp_hw_support
    PRIV_REQUIRES usb usb_device esp_driver_jpeg esp_timer
)
