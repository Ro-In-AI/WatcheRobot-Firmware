idf_component_register(
    SRCS "src/ble_service.c"
    INCLUDE_DIRS "include"
    # Phase 6: BLE GATT + WiFi Provisioning (this iteration only migrates motion control)
    REQUIRES "bt" "nvs_flash" "hal_servo" "wifi_manager" "json" "behavior_state_service"
)
