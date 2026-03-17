idf_component_register(
    SRCS "src/hal_camera.c"
    INCLUDE_DIRS "include"
    REQUIRES sensecap-watcher sscma_client mbedtls esp_timer
)
