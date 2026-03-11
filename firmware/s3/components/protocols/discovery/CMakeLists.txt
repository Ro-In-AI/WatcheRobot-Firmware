idf_component_register(
    SRCS "src/discovery_client.c"
    INCLUDE_DIRS "include"
    REQUIRES "esp_netif" "lwip"
    PRIV_REQUIRES  "esp_timer"
)
