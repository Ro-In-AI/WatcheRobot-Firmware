cmake_minimum_required(VERSION 3.16)

# Extra component directories (nested structure)
# ESP-IDF only scans components/ directly, so we need to specify subdirectories
set(EXTRA_COMPONENT_DIRS
    # Drivers
    "components/drivers/bsp_watcher"
    # HAL
    "components/hal/hal_audio"
    "components/hal/hal_button"
    "components/hal/hal_camera"
    "components/hal/hal_display"
    "components/hal/hal_servo"
    # Protocols
    "components/protocols/ble_service"
    "components/protocols/discovery"
    "components/protocols/ws_client"
    # Services
    "components/services/anim_service"
    "components/services/camera_service"
    "components/services/ota_service"
    "components/services/voice_service"
    # Utils
    "components/utils/boot_anim"
    "components/utils/wifi_manager"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(WatcheRobot-S3)

# Create SPIFFS partition image from spiffs/ directory
spiffs_create_partition_image(storage spiffs FLASH_IN_PROJECT)
