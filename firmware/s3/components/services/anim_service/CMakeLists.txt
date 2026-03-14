idf_component_register(
    SRCS
        "src/anim_storage.c"
        "src/anim_player.c"
        "src/anim_meta.c"
        "src/display_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES
        "lvgl"
        "spiffs"
        "json"
    PRIV_REQUIRES
        "sensecap-watcher"
        "esp_lvgl_port"
        "hal_display"
)
