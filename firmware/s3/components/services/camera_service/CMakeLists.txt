idf_component_register(
    SRCS "src/camera_service.c"
    INCLUDE_DIRS "include"
    REQUIRES "hal_camera"
    PRIV_REQUIRES "esp_log" "freertos"
)
