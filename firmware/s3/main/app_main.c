#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_task_wdt.h"

#include "bsp_watcher.h"
#include "camera_service.h"
#include "voice_service.h"
#include "display_ui.h"
#include "wifi_manager.h"
#include "ws_client.h"
#include "ws_handlers.h"
#include "ws_router.h"
#include "discovery_client.h"
#include "ble_service.h"
#include "hal_display.h"
#include "hal_servo.h"
#include "boot_anim.h"
#include "anim_storage.h"
#include "ota_service.h"
#include "sensecap-watcher.h"

#define TAG "MAIN"

/* Physical restart: click count to trigger reboot */
#define RESTART_CLICK_COUNT  5

static bool s_waiting_for_wifi_provision = false;
static bool s_ble_only_mode = false;

static void on_wifi_status_changed(wifi_status_t status, const char *ssid, const char *ip_addr)
{
    switch (status) {
        case WIFI_STATUS_CONNECTED:
            ESP_LOGI(TAG, "WiFi provisioning success: ssid=%s ip=%s",
                     ssid ? ssid : "<unknown>",
                     ip_addr ? ip_addr : "<no-ip>");
            if (s_waiting_for_wifi_provision || s_ble_only_mode) {
                display_update(ip_addr ? ip_addr : "WiFi Connected", "happy", 0, NULL);
            }
            break;

        case WIFI_STATUS_CONNECTING:
            ESP_LOGI(TAG, "WiFi connecting: ssid=%s", ssid ? ssid : "<unknown>");
            if (s_waiting_for_wifi_provision) {
                boot_anim_set_text("Connecting WiFi...");
            } else if (s_ble_only_mode) {
                display_update(ssid ? ssid : "Connecting WiFi...", "analyzing", 0, NULL);
            }
            break;

        case WIFI_STATUS_DISCONNECTED:
            ESP_LOGW(TAG, "WiFi connect failed or disconnected: ssid=%s",
                     ssid ? ssid : "<unknown>");
            if (s_waiting_for_wifi_provision) {
                boot_anim_set_text("WiFi Failed, Retry");
            } else if (s_ble_only_mode) {
                display_update("WiFi Failed, Retry", "sad", 0, NULL);
            }
            break;

        case WIFI_STATUS_UNCONFIGURED:
        default:
            ESP_LOGI(TAG, "WiFi unconfigured, waiting for BLE provisioning");
            if (s_waiting_for_wifi_provision) {
                boot_anim_set_text("Open APP Set WiFi");
            } else if (s_ble_only_mode) {
                display_update("Open APP Set WiFi", "standby", 0, NULL);
            }
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Button Callbacks (using SDK's bsp_set_btn_* interface)             */
/* ------------------------------------------------------------------ */

static void on_button_long_press(void)
{
    ESP_LOGI(TAG, "Button LONG PRESS - start recording");
    voice_recorder_process_event(VOICE_EVENT_BUTTON_PRESS);
    display_update("Listening...", "listening", 0, NULL);
}

static void on_button_long_release(void)
{
    ESP_LOGI(TAG, "Button LONG RELEASE - stop recording");
    voice_recorder_process_event(VOICE_EVENT_BUTTON_RELEASE);
    display_update("Processing...", "analyzing", 0, NULL);
}

static void on_button_multi_click_restart(void)
{
    ESP_LOGW(TAG, "Button %d-click - REBOOTING!", RESTART_CLICK_COUNT);
    display_update("Rebooting...", "sad", 0, NULL);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

/* ------------------------------------------------------------------ */
/* Emoji loading progress callback                                    */
/* ------------------------------------------------------------------ */

static void on_emoji_type_loaded(emoji_anim_type_t type, int types_done, int types_total)
{
    int progress = 5 + (types_done * 15) / types_total;
    boot_anim_set_progress(progress);
    boot_anim_set_text(emoji_type_name(type));
}

static void run_camera_boot_diag(void)
{
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

void app_main(void)
{
    ESP_LOGI(TAG, "WatcheRobot S3 v2.0 starting");
    bool cloud_ready = false;

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

    /* 6. Register button callbacks */
    bsp_set_btn_long_press_cb(on_button_long_press);
    bsp_set_btn_long_release_cb(on_button_long_release);
    bsp_set_btn_multi_click_cb(RESTART_CLICK_COUNT, on_button_multi_click_restart);

    /* 7. WiFi stack init */
    boot_anim_set_progress(40);
    boot_anim_set_text("WiFi...");
    wifi_init();
    wifi_register_status_callback(on_wifi_status_changed);

    /* 7.5 Start BLE service so app can provision WiFi if needed */
    boot_anim_set_progress(45);
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

    /* 8. Connect WiFi or wait for BLE provisioning */
    if (wifi_connect() != 0) {
        s_waiting_for_wifi_provision = true;
        boot_anim_set_text("Open APP Set WiFi");
        ESP_LOGI(TAG, "Waiting for WiFi credentials via BLE provisioning");
        if (wifi_wait_for_connection(-1) != 0) {
            ESP_LOGE(TAG, "Waiting for WiFi connection failed");
            return;
        }
        s_waiting_for_wifi_provision = false;
    }
    boot_anim_set_progress(55);

    /* 9. Service discovery */
    boot_anim_set_text("Discovering...");
    discovery_init();
    server_info_t server_info = {0};
    if (discovery_start(&server_info) == 0) {
        ESP_LOGI(TAG, "Server: %s:%u", server_info.ip, server_info.port);
        boot_anim_set_progress(65);
        char *ws_url = discovery_get_ws_url(&server_info);
        if (ws_url) {
            ws_client_set_server_url(ws_url);
            free(ws_url);
            cloud_ready = true;
        }
    } else {
        ESP_LOGW(TAG, "Discovery failed, continuing in BLE-only mode");
    }

    if (cloud_ready) {
        /* 10. Start voice recorder */
        if (voice_recorder_start() != 0) {
            ESP_LOGE(TAG, "Failed to start voice recorder (non-fatal)");
        }

        /* 11. WebSocket client */
        boot_anim_set_progress(92);
        boot_anim_set_text("Connecting...");
        ws_client_init();
        ws_router_t router = ws_handlers_get_router();
        ws_router_init(&router);
    } else {
        boot_anim_set_progress(100);
        boot_anim_set_text("BLE Ready");
        s_ble_only_mode = true;
    }

    /* 12. Ready! */
    if (cloud_ready) {
        boot_anim_set_progress(100);
        boot_anim_set_text("Ready!");
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    boot_anim_finish();
    hal_display_ui_init();
    run_camera_boot_diag();
    if (cloud_ready) {
        ws_client_start();
    } else {
        display_update("BLE Ready", "standby", 0, NULL);
    }
    /* Note: hal_display_ui_init() already sets "Ready" text and starts default animation.
     * Don't call display_update here as it would override the startup UI state. */
    ESP_LOGI(TAG, "WatcheRobot ready (cloud=%s, ble=%s)",
             cloud_ready ? "online" : "offline",
             ble_service_is_connected() ? "connected" : "advertising");

    /* 13. Mark OTA partition valid (prevent rollback after successful boot) */
    ota_service_mark_valid();

    /* Main loop */
    esp_task_wdt_add(NULL);
    while (1) {
        esp_task_wdt_reset();
        ws_tts_timeout_check();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
