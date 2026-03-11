idf_component_register(
    SRCS "src/ble_service.c"
    INCLUDE_DIRS "include"
    REQUIRES "esp_err"
    PRIV_REQUIRES "esp_log"
)
