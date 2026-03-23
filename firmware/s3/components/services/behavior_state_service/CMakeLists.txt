idf_component_register(
    SRCS "src/behavior_state_service.c"
    INCLUDE_DIRS "include"
    REQUIRES
        "anim_service"
        "freertos"
        "hal_servo"
        "json"
        "sfx_service"
    PRIV_REQUIRES
        "esp_timer"
)
