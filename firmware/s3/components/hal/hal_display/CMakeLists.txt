idf_component_register(
    SRCS "src/hal_display.c"
    INCLUDE_DIRS "include"
    # NOTE: anim_service and boot_anim are services/utils layer — temporary layer
    # violation kept from MVP-W. Will be decoupled in Phase 3 (anim_service refactor).
    PRIV_REQUIRES
        "hal_button"
        "sensecap-watcher"
        "lvgl"
        "esp_lvgl_port"
        "anim_service"
        "boot_anim"
)
