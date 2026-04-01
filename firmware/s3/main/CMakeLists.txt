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
        ble_service
        # Services
        voice_service
        behavior_state_service
        anim_service
        camera_service
        control_ingress
        ota_service
        # Utils
        wifi_manager
        boot_anim
        # ESP-IDF
        sensecap-watcher
)
