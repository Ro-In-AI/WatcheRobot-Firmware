idf_component_register(
    SRCS
        "src/ws_client.c"
        "src/ws_router.c"
        "src/ws_handlers.c"
    INCLUDE_DIRS "include"
    REQUIRES "json" "esp_event"
    PRIV_REQUIRES
        "camera_service"
        "control_ingress"
        "esp_wifi"
        "freertos"
        "hal_audio"
        "hal_servo"
        "ota_service"
        "behavior_state_service"
        "sfx_service"
        "voice_service"
        "anim_service"

)
