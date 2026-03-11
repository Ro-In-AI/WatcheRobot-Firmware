idf_component_register(
    SRCS "src/ble_service.c"
    INCLUDE_DIRS "include"
    # Phase 6: BLE GATT + WiFi Provisioning
    REQUIRES "bt" "nvs_flash"
)
