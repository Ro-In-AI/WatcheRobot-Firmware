cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "0.1.2alpha")

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
    "components/services/behavior_state_service"
    "components/services/camera_service"
    "components/services/control_ingress"
    "components/services/ota_service"
    "components/services/sfx_service"
    "components/services/voice_service"
    # Utils
    "components/utils/boot_anim"
    "components/utils/wifi_manager"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(WatcheRobot-S3)

if(DEFINED CONFIG_WATCHER_ANIM_FPS)
    set(ANIM_DEFAULT_FPS ${CONFIG_WATCHER_ANIM_FPS})
else()
    set(ANIM_DEFAULT_FPS 10)
endif()

set(ANIM_IMPORT_DIR ${CMAKE_SOURCE_DIR}/assets/watcher0327png)

add_custom_target(anim_assets_gen
    COMMAND py -3 ${CMAKE_SOURCE_DIR}/tools/generate_anim_assets.py
            --spiffs-dir ${CMAKE_SOURCE_DIR}/spiffs
            --output-dir ${CMAKE_SOURCE_DIR}/spiffs/anim
            --import-dir ${ANIM_IMPORT_DIR}
            --fps ${ANIM_DEFAULT_FPS}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    BYPRODUCTS
        ${CMAKE_SOURCE_DIR}/spiffs/anim/anim_manifest.bin
    COMMENT "Generating animation manifest and raw565 previews")

# Create SPIFFS partition image from spiffs/ directory
spiffs_create_partition_image(storage spiffs FLASH_IN_PROJECT DEPENDS anim_assets_gen)
