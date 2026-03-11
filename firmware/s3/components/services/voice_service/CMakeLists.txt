idf_component_register(
    SRCS
        "src/voice_fsm.c"
        "src/hal_wake_word.c"
    INCLUDE_DIRS "include"
    REQUIRES "hal_audio" "freertos"
    PRIV_REQUIRES
        "sensecap-watcher"
        "esp-sr"
        "esp_timer"
        
)
