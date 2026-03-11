idf_component_register(
    SRCS "src/hal_camera.c"
    INCLUDE_DIRS "include"
    REQUIRES "esp_err"
    PRIV_REQUIRES "esp_log"
)
