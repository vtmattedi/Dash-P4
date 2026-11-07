# MIPI-CSI camera capture (see camera.h). Product-agnostic: no LVGL, no USB, no NightMare.
idf_component_register(
    SRCS camera.c
    INCLUDE_DIRS "."
    REQUIRES driver esp_driver_i2c
    PRIV_REQUIRES esp_video esp_mm esp_timer
)

# esp_ipa 2.0.0 ships its prebuilt libesp_ipa.a with a *dummy* esp_video_ipa_config.c.obj (the library's test app) that
# defines esp_ipa_pipeline_get_config() too, and the component compiles the real one, generated from the sensors'
# tuning files, into libespressif__esp_ipa.a. Both are strong definitions of one symbol and the linker keeps the first it
# extracts. Inside the link's --start-group the generated archive is scanned *before* esp_video asks for the function
# (esp_video comes later in the group), so it is skipped and the dummy, scanned last, wins. Then esp_video finds no
# tuning for "OV02C10" ("failed to get configuration to initialize ISP controller"), never starts the ISP pipeline, and
# the picture is untuned. Whether it happened changed from build to build.
# An undefined reference at the very start of the link makes the generated archive supply the definition the first time
# it is scanned, so the dummy is never extracted. (WHOLE_ARCHIVE does not help: PlatformIO builds the final link itself
# and drops the position of --whole-archive.)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u esp_ipa_pipeline_get_config")
