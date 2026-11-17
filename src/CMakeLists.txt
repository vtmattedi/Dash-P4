# The dashboard: every source under src/. (The USB camera and the Windows USB screen that used to be built
# from here were proofs of concept; they are in git history under the tag usb-proofs.)
file(GLOB_RECURSE app_sources
    ${CMAKE_CURRENT_LIST_DIR}/*.c
    ${CMAKE_CURRENT_LIST_DIR}/*.cpp
)

set(app_requires board)
set(app_priv_requires espressif__esp_hosted app_update esp_http_client esp_https_ota esp-tls mbedtls lwip espressif__led_strip esp_driver_rmt)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
    REQUIRES ${app_requires}
    PRIV_REQUIRES ${app_priv_requires}
)
