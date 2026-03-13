idf_component_register(
    SRCS
        "src/ws_client.c"
        "src/ws_router.c"
        "src/ws_handlers.c"
    INCLUDE_DIRS "include"
    REQUIRES "json" "esp_event"
    PRIV_REQUIRES
        "hal_audio"
        "hal_servo"
        "voice_service"
        "anim_service"

)
