idf_component_register(
    SRCS
        "src/anim_storage.c"
        "src/anim_player.c"
        "src/display_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES "lvgl" "spiffs"
    PRIV_REQUIRES
        "sensecap-watcher"
        "esp_lvgl_port"
        
)
