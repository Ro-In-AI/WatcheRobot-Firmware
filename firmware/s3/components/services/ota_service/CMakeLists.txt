idf_component_register(
    SRCS "src/ota_service.c"
    INCLUDE_DIRS "include"
    REQUIRES "esp_ota_ops" "esp_http_client"
    PRIV_REQUIRES  "esp_timer"
)
