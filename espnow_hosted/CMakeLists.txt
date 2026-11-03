# ESP-NOW for the P4 through the C6 (see esp_now_hosted.c). Product-agnostic: any ESP-NOW client links against it.
idf_component_register(
    SRCS esp_now_hosted.c
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_wifi espressif__esp_wifi_remote espressif__esp_hosted
)

# The esp_now_* definitions are only referenced by libraries linked after the IDF components (PlatformIO's lib_deps);
# whole-archive keeps them in the link whatever the order.
idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)
