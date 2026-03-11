idf_component_register(
    SRCS "src/wifi_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES "esp_wifi" "esp_netif" "esp_event" "nvs_flash"
    PRIV_REQUIRES  "freertos"
)
