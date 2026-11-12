cmake_minimum_required(VERSION 3.16.0)

# board/ is the reusable per-board component (pins, display, touch, LVGL port). Its Kconfig defaults sit beside it and
# are merged under the project's own.
# PlatformIO only watches sdkconfig.defaults; src/Version/script.py deletes the generated
# sdkconfig.<env> when any defaults file is newer, so all of them take effect on the next build.
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/board")
# espnow_hosted/ (the esp_now_* API carried out by the C6) only when the env asks for ESP-NOW (custom_dash_espnow):
# it links whole-archive, and it has no managed dependencies of its own, so leaving it out changes nothing else.
if("$ENV{DASH_ESPNOW}" STREQUAL "1")
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/espnow_hosted")
    message(STATUS "Dash-P4: ESP-NOW over the C6 (espnow_hosted)")
endif()
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/board/sdkconfig.board;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")

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
