# The firmware is chosen by DASH_APP (see the root CMakeLists.txt). Read from the environment here as well:
# this file also runs in IDF's early requirement pass, which does not see the root's variables.
set(dash_app "$ENV{DASH_APP}")

set(app_requires board usb_device usb_screen)

if(dash_app STREQUAL "usb_screen")
    # The USB screen alone: its status screen and the panel-ownership logic, no NightMare.
    file(GLOB_RECURSE app_sources
        ${CMAKE_CURRENT_LIST_DIR}/UsbScreen/*.cpp
        ${CMAKE_CURRENT_LIST_DIR}/PcLink/*.cpp
    )
    set(app_priv_requires "")
elseif(dash_app STREQUAL "usb_cam")
    # The board's camera on the panel: no radio, no NightMare, no PC link.
    file(GLOB_RECURSE app_sources ${CMAKE_CURRENT_LIST_DIR}/UsbCam/*.cpp)
    set(app_priv_requires esp_mm esp_driver_ppa esp_timer esp_psram)
    set(app_requires board camera usb_camera usb_device)
else()
    file(GLOB_RECURSE app_sources
        ${CMAKE_CURRENT_LIST_DIR}/*.c
        ${CMAKE_CURRENT_LIST_DIR}/*.cpp
    )
    list(FILTER app_sources EXCLUDE REGEX "/UsbScreen/|/UsbCam/")
    if(NOT dash_app STREQUAL "dashboard_usb")
        list(FILTER app_sources EXCLUDE REGEX "/PcLink/|/App/PcScreen\\.cpp$")
    endif()
    set(app_priv_requires espressif__esp_hosted app_update esp_http_client esp_https_ota esp-tls)
    if("$ENV{DASH_CAMERA}" STREQUAL "1")
        # The dashboard with the camera (custom_dash_camera): the camera service and its page.
        list(APPEND app_requires camera usb_camera)
        list(APPEND app_priv_requires esp_driver_ppa esp_mm esp_timer)
        set(dash_has_camera ON)
    else()
        list(FILTER app_sources EXCLUDE REGEX "/App/CameraMode\\.cpp$|/Screens/PageCamera\\.cpp$")
    endif()
endif()

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
    REQUIRES ${app_requires}
    PRIV_REQUIRES ${app_priv_requires}
)

if(dash_app STREQUAL "dashboard_usb")
    # The dashboard's PC-screen glue (App/PcScreen, the Settings section) compiles in with this.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DASH_PC_SCREEN=1)
endif()

if(dash_has_camera)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DASH_CAMERA=1)
endif()
