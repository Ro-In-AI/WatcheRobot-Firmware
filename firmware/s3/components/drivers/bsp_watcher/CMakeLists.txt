idf_component_register(
    SRCS "src/bsp_watcher.c"
    INCLUDE_DIRS "include"
    REQUIRES "sensecap-watcher"
    PRIV_REQUIRES "esp_log" "driver"
)
