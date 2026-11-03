# The firmware is chosen by DASH_APP (see the root CMakeLists.txt). Read from the environment here as well:
# this file also runs in IDF's early requirement pass, which does not see the root's variables.
set(dash_app "$ENV{DASH_APP}")

if(dash_app STREQUAL "usb_screen")
    # The USB screen alone: its status screen and the panel-ownership logic, no NightMare.
    file(GLOB_RECURSE app_sources
        ${CMAKE_CURRENT_LIST_DIR}/UsbScreen/*.cpp
        ${CMAKE_CURRENT_LIST_DIR}/PcLink/*.cpp
    )
    set(app_priv_requires "")
else()
    file(GLOB_RECURSE app_sources
        ${CMAKE_CURRENT_LIST_DIR}/*.c
        ${CMAKE_CURRENT_LIST_DIR}/*.cpp
    )
    list(FILTER app_sources EXCLUDE REGEX "/UsbScreen/")
    if(NOT dash_app STREQUAL "dashboard_usb")
        list(FILTER app_sources EXCLUDE REGEX "/PcLink/|/App/PcScreen\\.cpp$")
    endif()
    set(app_priv_requires espressif__esp_hosted app_update esp_http_client esp_https_ota esp-tls)
endif()

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
    REQUIRES board usb_screen
    PRIV_REQUIRES ${app_priv_requires}
)

if(dash_app STREQUAL "dashboard_usb")
    # The dashboard's PC-screen glue (App/PcScreen, the Settings section) compiles in with this.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DASH_PC_SCREEN=1)
endif()
