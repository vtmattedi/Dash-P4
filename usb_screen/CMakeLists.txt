# USB extended display for Windows (see usb_screen.h and NOTES.md). Product-agnostic: no LVGL, no NightMare.
idf_component_register(
    SRCS usb_screen.c usb_screen_video.c usb_screen_touch.c
    INCLUDE_DIRS "." "tusb"
    REQUIRES esp_lcd
    PRIV_REQUIRES usb esp_driver_jpeg esp_timer esp_hw_support
)

# TinyUSB is configured by this component, as in Espressif's example: the stack reads tusb_config.h from our
# tusb/ folder and calls the descriptor callbacks in usb_descriptors.c, so both go into its own target.
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PUBLIC "${CMAKE_CURRENT_LIST_DIR}/tusb")
target_sources(${tusb_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/tusb/usb_descriptors.c")
cmake_policy(SET CMP0079 NEW)
target_link_libraries(${tusb_lib} PRIVATE ${COMPONENT_LIB})
