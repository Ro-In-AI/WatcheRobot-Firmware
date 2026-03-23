idf_component_register(
    SRCS
        "src/voice_fsm.c"
        "src/hal_wake_word.c"
    INCLUDE_DIRS "include"
    REQUIRES "hal_audio" "hal_button" "freertos"
    PRIV_REQUIRES
        "sensecap-watcher"
        "esp_timer"
        "ws_client"
        "behavior_state_service"
        "anim_service"

)
