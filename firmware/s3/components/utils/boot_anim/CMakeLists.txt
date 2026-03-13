idf_component_register(
    SRCS "src/boot_anim.c"
    INCLUDE_DIRS "include"
    REQUIRES "lvgl"
    PRIV_REQUIRES
        "sensecap-watcher"
        "esp_lvgl_port"
        "anim_service"
)
