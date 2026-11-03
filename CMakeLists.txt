cmake_minimum_required(VERSION 3.16.0)

# Which firmware this build is (platformio.ini: custom_dash_app). src/Version/script.py hands it over as an
# environment variable, not a CMake one, because IDF's early component-requirement pass runs in a separate
# CMake process that sees the environment but not the cache.
#   dashboard      the NightMare wall panel (default)
#   usb_screen     the panel as a Windows USB extended display, nothing else (no radio, no NightMare)
#   dashboard_usb  the dashboard, and the Windows display whenever the PC streams to it
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
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/board" "${CMAKE_CURRENT_LIST_DIR}/usb_screen")
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

# Fixed so project.cmake never shells out to `git describe`: that fails (and breaks
# the configure step) on a repository with no commits, and the real version
# string comes from include/Version.h anyway.
set(PROJECT_VER "0.1.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(DashP4)
