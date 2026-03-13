idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        # Drivers
        bsp_watcher
        # HAL
        hal_audio
        hal_display
        hal_servo
        # Protocols
        ws_client
        discovery
        # Services
        voice_service
        anim_service
        ota_service
        # Utils
        wifi_manager
        boot_anim
        # ESP-IDF
        sensecap-watcher
)
