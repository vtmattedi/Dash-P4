# The USB device of the high-speed port (see usb_device.h). Product-agnostic: no LVGL, no NightMare.
idf_component_register(
    SRCS usb_device.c
    INCLUDE_DIRS "." "tusb"
    PRIV_REQUIRES usb
)

# TinyUSB is configured by this component: the stack reads tusb_config.h from our tusb/ folder, so the folder goes on
# the tinyusb target's include path. (The descriptor callbacks the stack calls are in usb_device.c and dispatch to the
# registered personalities.)
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PUBLIC "${CMAKE_CURRENT_LIST_DIR}/tusb")
cmake_policy(SET CMP0079 NEW)
target_link_libraries(${tusb_lib} PRIVATE ${COMPONENT_LIB})
