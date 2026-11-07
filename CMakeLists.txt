cmake_minimum_required(VERSION 3.16.0)

# Which firmware this build is (platformio.ini: custom_dash_app). src/Version/script.py hands it over as an
# environment variable, not a CMake one, because IDF's early component-requirement pass runs in a separate
# CMake process that sees the environment but not the cache.
#   dashboard      the NightMare wall panel (default)
#   usb_screen     the panel as a Windows USB extended display, nothing else (no radio, no NightMare)
#   dashboard_usb  the dashboard, and the Windows display whenever the PC streams to it
#   usb_cam        the board's own MIPI-CSI camera (stage 1 of the USB camera: preview on the panel, then UVC)
set(DASH_APP "$ENV{DASH_APP}")
if(NOT DASH_APP)
    set(DASH_APP dashboard)
endif()
message(STATUS "Dash-P4 firmware: ${DASH_APP}")

# board/ is the reusable per-board component (pins, display, touch, LVGL port); usb_screen/ the reusable USB
# extended display. Both are in every build so the managed-component set (and dependencies.lock) is the same
# for every env; the linker drops what a firmware does not call.
# Their Kconfig defaults sit beside them and are merged under the project's own.
# PlatformIO only watches sdkconfig.defaults; src/Version/script.py deletes the generated
# sdkconfig.<env> when any defaults file is newer, so all of them take effect on the next build.
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/board" "${CMAKE_CURRENT_LIST_DIR}/usb_device"
     "${CMAKE_CURRENT_LIST_DIR}/usb_screen")
# The camera (camera/, usb_camera/) is only in the envs that ask for it (DASH_CAMERA, set by src/Version/script.py: the
# camera firmware, or custom_dash_camera = yes). IDF builds every component it finds, and esp_video (with esp_ipa,
# esp_h264, usb_host_uvc) is large, wants a sensor selected in Kconfig, and does not even compile without one (esp_ipa's
# generated stub lacks <stddef.h>). Those envs' managed dependencies get a lock file of their own, so switching envs does
# not rewrite dependencies.lock.
if("$ENV{DASH_CAMERA}" STREQUAL "1")
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/camera" "${CMAKE_CURRENT_LIST_DIR}/usb_camera")
    # The registry's esp_cam_sensor (every release up to 2.6.0) has no OV02C10 driver, the sensor on this board. The
    # vendor's demo carries its own copy that has; a local component of the same name overrides the registry's one
    # for esp_video's dependency (third_party/NOTES.md).
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/third_party/esp_cam_sensor")
    # esp_video is patched too: the ISP tuning loop rewrites saturation, contrast and white balance every frame, so the
    # user's adjustments have to be applied inside its pipeline controller (third_party/NOTES.md).
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/third_party/esp_video")
    set(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.camera.lock")
endif()
# espnow_hosted/ (the esp_now_* API carried out by the C6) only when the env asks for ESP-NOW (custom_dash_espnow):
# it links whole-archive, and it has no managed dependencies of its own, so leaving it out changes nothing else.
if("$ENV{DASH_ESPNOW}" STREQUAL "1")
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/espnow_hosted")
    message(STATUS "Dash-P4: ESP-NOW over the C6 (espnow_hosted)")
endif()
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/board/sdkconfig.board;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
if(DASH_APP STREQUAL "usb_screen")
    list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/src/UsbScreen/sdkconfig.usb_screen")
endif()
if("$ENV{DASH_CAMERA}" STREQUAL "1")
    # The camera sensor and mode (src/UsbCam/sdkconfig.camera); the camera firmware also needs no radio.
    list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/src/UsbCam/sdkconfig.camera")
endif()
if(DASH_APP STREQUAL "usb_cam")
    list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/src/UsbCam/sdkconfig.usb_cam")
endif()

# Fixed so project.cmake never shells out to `git describe`: that fails (and breaks
# the configure step) on a repository with no commits, and the real version
# string comes from include/Version.h anyway.
set(PROJECT_VER "0.1.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
if(DEPENDENCIES_LOCK)
    # The component manager reads the lock path from this build property.
    idf_build_set_property(DEPENDENCIES_LOCK "${DEPENDENCIES_LOCK}")
endif()
project(DashP4)
