# Reusable board support for the Guition JC1060P470C. Product-agnostic: no
# NightMare, no application strings, nothing above LVGL initialisation.
file(GLOB board_sources ${CMAKE_CURRENT_LIST_DIR}/*.c ${CMAKE_CURRENT_LIST_DIR}/*.cpp)

idf_component_register(
    SRCS ${board_sources}
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd esp_driver_ledc esp_driver_i2c
    PRIV_REQUIRES esp_timer esp_psram esp_mm
)
