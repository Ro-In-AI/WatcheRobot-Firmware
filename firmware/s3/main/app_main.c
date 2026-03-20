#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "anim_storage.h"
#include "ble_service.h"
#include "boot_anim.h"
#include "bsp_watcher.h"
#include "camera_service.h"
#include "discovery_client.h"
#include "display_ui.h"
#include "hal_display.h"
#include "hal_servo.h"
#include "ota_service.h"
#include "sensecap-watcher.h"
#include "voice_service.h"
#include "wifi_manager.h"
#include "ws_client.h"
#include "ws_handlers.h"
#include "ws_router.h"

#include <stdlib.h>

#define TAG "MAIN"

/* Physical restart: click count to trigger reboot */
#define RESTART_CLICK_COUNT 5

/* ------------------------------------------------------------------ */
/* Button Callbacks (using SDK's bsp_set_btn_* interface)             */
/* ------------------------------------------------------------------ */

static void on_button_long_press(void) {
    ESP_LOGI(TAG, "Button LONG PRESS - start recording");
    voice_recorder_process_event(VOICE_EVENT_BUTTON_PRESS);
    display_update("Listening...", "listening", 0, NULL);
}

static void on_button_long_release(void) {
    ESP_LOGI(TAG, "Button LONG RELEASE - stop recording");
    voice_recorder_process_event(VOICE_EVENT_BUTTON_RELEASE);
    display_update("Processing...", "analyzing", 0, NULL);
}

static void on_button_multi_click_restart(void) {
    ESP_LOGW(TAG, "Button %d-click - REBOOTING!", RESTART_CLICK_COUNT);
    display_update("Rebooting...", "sad", 0, NULL);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

/* ------------------------------------------------------------------ */
/* Emoji loading progress callback                                    */
/* ------------------------------------------------------------------ */

static void on_emoji_type_loaded(emoji_anim_type_t type, int types_done, int types_total) {
    int progress = 5 + (types_done * 15) / types_total;
    boot_anim_set_progress(progress);
    boot_anim_set_text(emoji_type_name(type));
}

static void run_camera_boot_diag(void) {
#if CONFIG_WATCHER_CAMERA_BOOT_DIAG
    esp_err_t ret;

    ESP_LOGI(TAG, "Camera boot diagnostic: begin");
    ret = camera_service_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Camera boot diagnostic init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = camera_service_capture_once();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Camera boot diagnostic capture failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "Camera boot diagnostic: capture succeeded");
#endif
}

/* ------------------------------------------------------------------ */
/* Main Application                                                   */
/* ------------------------------------------------------------------ */

void app_main(void) {
    ESP_LOGI(TAG, "WatcheRobot S3 v2.0 starting");

    /* 1. Minimal display init for boot animation */
    if (hal_display_minimal_init() != 0) {
        ESP_LOGE(TAG, "Failed to initialize display");
        return;
    }

    /* 2. Show boot animation */
    boot_anim_init();
    boot_anim_set_text("Initializing...");
    boot_anim_set_progress(0);

    /* 3. Load animation assets first so boot intro can play immediately */
    boot_anim_set_text("Boot...");
    if (emoji_spiffs_init() == 0) {
        if (emoji_load_type(EMOJI_ANIM_BOOT) > 0) {
            /* Dedicated startup sequence: boot1.png ~ boot4.png */
            boot_anim_start_intro(EMOJI_ANIM_BOOT, 4, 120);
        }
        boot_anim_set_text("Loading...");
        emoji_load_all_images_with_cb(on_emoji_type_loaded);
    } else {
        ESP_LOGW(TAG, "SPIFFS init failed (emoji disabled)");
    }

    /* 4. Servo HAL init (GPIO 19/20 LEDC PWM, Phase 2 implementation) */
    boot_anim_set_progress(25);
    boot_anim_set_text("Servo...");
    hal_servo_init();

    /* 5. Voice recorder: init only (do NOT start yet) */
    boot_anim_set_progress(30);
    boot_anim_set_text("Voice...");
    voice_recorder_init();

    /* 5.5 BLE motion control (optional, no provisioning in this iteration) */
    boot_anim_set_progress(35);
    boot_anim_set_text("BLE...");
    esp_err_t ble_ret = ble_service_init();
    if (ble_ret == ESP_OK) {
        ble_ret = ble_service_start_advertising();
        if (ble_ret != ESP_OK) {
            ESP_LOGW(TAG, "BLE advertising start failed: %s", esp_err_to_name(ble_ret));
        }
    } else if (ble_ret != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "BLE init failed: %s", esp_err_to_name(ble_ret));
    }

    /* 6. Register button callbacks */
    bsp_set_btn_long_press_cb(on_button_long_press);
    bsp_set_btn_long_release_cb(on_button_long_release);
    bsp_set_btn_multi_click_cb(RESTART_CLICK_COUNT, on_button_multi_click_restart);

    /* 7. WiFi */
    boot_anim_set_progress(40);
    boot_anim_set_text("WiFi...");
    wifi_init();
    if (wifi_connect() != 0) {
        boot_anim_show_error("WiFi Error");
        return;
    }
    boot_anim_set_progress(55);

    /* 8. Service discovery */
    boot_anim_set_text("Discovering...");
    discovery_init();
    server_info_t server_info = {0};
    if (discovery_start(&server_info) != 0) {
        boot_anim_show_error("Server Not Found");
        return;
    }
    if (server_info.protocol_version[0] == '\0' ||
        strcmp(server_info.protocol_version, WATCHER_PROTOCOL_VERSION) != 0) {
        ESP_LOGE(TAG,
                 "Protocol mismatch: server=%s expected=%s",
                 server_info.protocol_version[0] != '\0' ? server_info.protocol_version : "<missing>",
                 WATCHER_PROTOCOL_VERSION);
        boot_anim_show_error("Protocol Mismatch");
        return;
    }

    ESP_LOGI(TAG, "Server: %s:%u protocol=%s", server_info.ip, server_info.port, server_info.protocol_version);
    boot_anim_set_progress(65);
    char *ws_url = discovery_get_ws_url(&server_info);
    if (ws_url) {
        ws_client_set_server_url(ws_url);
        free(ws_url);
    }

    /* 9. Start voice recorder */
    if (voice_recorder_start() != 0) {
        ESP_LOGE(TAG, "Failed to start voice recorder (non-fatal)");
    }

    /* 10. WebSocket client */
    boot_anim_set_progress(92);
    boot_anim_set_text("Connecting...");
    ws_client_init();
    ws_handlers_init();
    ws_router_t router = ws_handlers_get_router();
    ws_router_init(&router);

    /* 11. Ready! */
    boot_anim_set_progress(100);
    boot_anim_set_text("Ready!");
    vTaskDelay(pdMS_TO_TICKS(500));
    boot_anim_finish();
    hal_display_ui_init();
    run_camera_boot_diag();
    ws_client_start();
    /* Note: hal_display_ui_init() already sets "Ready" text and starts default animation.
     * Don't call display_update here as it would override the startup UI state. */
    ESP_LOGI(TAG, "WatcheRobot ready");

    /* 12. Mark OTA partition valid (prevent rollback after successful boot) */
    ota_service_mark_valid();

    /* Main loop */
    esp_task_wdt_add(NULL);
    while (1) {
        esp_task_wdt_reset();
        ws_tts_timeout_check();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
