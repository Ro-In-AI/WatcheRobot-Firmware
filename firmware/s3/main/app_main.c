#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "anim_storage.h"
#include "behavior_state_service.h"
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
#include <string.h>

#define TAG "MAIN"

/* Physical restart: click count to trigger reboot */
#define RESTART_CLICK_COUNT 5
#define STARTUP_BEHAVIOR_POLL_MS 50
#define STARTUP_BEHAVIOR_TIMEOUT_MS 10000

static bool s_waiting_for_wifi_provision = false;
static bool s_ble_only_mode = false;

static void on_wifi_status_changed(wifi_status_t status, const char *ssid, const char *ip_addr) {
    switch (status) {
    case WIFI_STATUS_CONNECTED:
        ESP_LOGI(TAG, "WiFi provisioning success: ssid=%s ip=%s", ssid ? ssid : "<unknown>",
                 ip_addr ? ip_addr : "<no-ip>");
        if (s_waiting_for_wifi_provision) {
            boot_anim_set_text("WiFi Connected");
        } else if (s_ble_only_mode) {
            behavior_state_set_with_text("happy", ip_addr ? ip_addr : "WiFi Connected", 0);
        }
        break;

    case WIFI_STATUS_CONNECTING:
        ESP_LOGI(TAG, "WiFi connecting: ssid=%s", ssid ? ssid : "<unknown>");
        if (s_waiting_for_wifi_provision) {
            boot_anim_set_text("Connecting WiFi...");
        } else if (s_ble_only_mode) {
            behavior_state_set_with_text("processing", ssid ? ssid : "Connecting WiFi...", 0);
        }
        break;

    case WIFI_STATUS_DISCONNECTED:
        ESP_LOGW(TAG, "WiFi connect failed or disconnected: ssid=%s", ssid ? ssid : "<unknown>");
        if (s_waiting_for_wifi_provision) {
            boot_anim_set_text("WiFi Failed, Retry");
        } else if (s_ble_only_mode) {
            behavior_state_set_with_text("error", "WiFi Failed, Retry", 0);
        }
        break;

    case WIFI_STATUS_UNCONFIGURED:
    default:
        ESP_LOGI(TAG, "WiFi unconfigured, waiting for BLE provisioning");
        if (s_waiting_for_wifi_provision) {
            boot_anim_set_text("Open APP Set WiFi");
        } else if (s_ble_only_mode) {
            behavior_state_set_with_text("standby", "Open APP Set WiFi", 0);
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Button Callbacks (using SDK's bsp_set_btn_* interface)             */
/* ------------------------------------------------------------------ */

static void on_button_multi_click_restart(void) {
    ESP_LOGW(TAG, "Button %d-click - REBOOTING!", RESTART_CLICK_COUNT);
    behavior_state_set_with_text("error", "Rebooting...", 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
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

static void log_heap_state(const char *stage) {
    size_t free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t largest_8bit = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t largest_spiram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG,
             "Heap @ %s: 8bit=%u KB (largest %u KB), internal=%u KB (largest %u KB), psram=%u KB (largest %u KB)",
             stage,
             (unsigned)(free_8bit / 1024U),
             (unsigned)(largest_8bit / 1024U),
             (unsigned)(free_internal / 1024U),
             (unsigned)(largest_internal / 1024U),
             (unsigned)(free_spiram / 1024U),
             (unsigned)(largest_spiram / 1024U));
}

static int ensure_boot_wifi_connection(void) {
    char saved_ssid[33] = {0};
    if (wifi_has_credentials() != 1) {
        ESP_LOGI(TAG, "No stored WiFi credentials found");
        return -1;
    }

    if (wifi_get_saved_ssid(saved_ssid, sizeof(saved_ssid)) == 0) {
        ESP_LOGI(TAG, "Connecting to stored WiFi SSID: %s", saved_ssid);
    } else {
        ESP_LOGI(TAG, "Connecting to stored WiFi credentials");
    }

    if (wifi_connect() == 0) {
        return 0;
    }

    ESP_LOGW(TAG, "Stored WiFi connect timed out, waiting for BLE provisioning");
    return -1;
}

static void wait_for_behavior_idle(uint32_t timeout_ms) {
    uint32_t waited_ms = 0;

    while (behavior_state_is_busy() && waited_ms < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(STARTUP_BEHAVIOR_POLL_MS));
        waited_ms += STARTUP_BEHAVIOR_POLL_MS;
    }

    if (behavior_state_is_busy()) {
        ESP_LOGW(TAG, "Timed out waiting for startup behavior to settle after %lu ms", (unsigned long)waited_ms);
    }
}

/* ------------------------------------------------------------------ */
/* Main Application                                                   */
/* ------------------------------------------------------------------ */

void app_main(void) {
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

    /* 3. Initialize animation catalog and load boot assets only */
    boot_anim_set_text("Boot...");
    if (anim_catalog_init() == 0) {
        if (emoji_load_type(EMOJI_ANIM_BOOT) > 0) {
            /* Dedicated startup sequence: boot1.png ~ boot4.png */
            boot_anim_start_intro(EMOJI_ANIM_BOOT, 4, 120);
        }
        boot_anim_set_text("Preparing...");
    } else {
        ESP_LOGW(TAG, "SPIFFS init failed (emoji disabled)");
    }

    /* 4. Servo HAL init (GPIO 19/20 LEDC PWM, Phase 2 implementation) */
    boot_anim_set_progress(25);
    boot_anim_set_text("Servo...");
    hal_servo_init();

    /* 5. Initialize app state only. Input devices stay disabled during BLE provisioning. */
    boot_anim_set_progress(30);
    boot_anim_set_text("State...");
    behavior_state_init();

    /* 5.5 BLE control + provisioning */
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

    /* 6. WiFi */
    boot_anim_set_progress(40);
    boot_anim_set_text("WiFi...");
    wifi_init();
    wifi_register_status_callback(on_wifi_status_changed);
    if (ensure_boot_wifi_connection() != 0) {
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

    /* 7. Service discovery */
    boot_anim_set_text("Discovering...");
    discovery_init();
    server_info_t server_info = {0};
    if (discovery_start(&server_info) == 0) {
        if (server_info.protocol_version[0] == '\0' ||
            strcmp(server_info.protocol_version, WATCHER_PROTOCOL_VERSION) != 0) {
            ESP_LOGE(TAG, "Protocol mismatch: server=%s expected=%s",
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
            cloud_ready = true;
        }
    } else {
        ESP_LOGW(TAG, "Discovery failed, continuing in BLE-only mode");
    }

    if (cloud_ready) {
        /* 8. WebSocket client */
        boot_anim_set_progress(92);
        boot_anim_set_text("Connecting...");
        ws_client_init();
        ws_handlers_init();
        ws_router_t router = ws_handlers_get_router();
        ws_router_init(&router);
    } else {
        boot_anim_set_progress(100);
        boot_anim_set_text("BLE Ready");
        s_ble_only_mode = true;
    }

    /* 9. Ready! */
    if (cloud_ready) {
        boot_anim_set_progress(100);
        boot_anim_set_text("Ready!");
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    boot_anim_finish();
    log_heap_state("before_ui_init");
    hal_display_ui_init();
    if (cloud_ready) {
        if (hal_display_input_init() != 0) {
            ESP_LOGW(TAG, "Delayed display input initialization failed");
        }

        /* Register encoder callbacks only after delayed input devices are attached. */
        bsp_set_btn_multi_click_cb(RESTART_CLICK_COUNT, on_button_multi_click_restart);

        voice_recorder_init();
        if (voice_recorder_start() != 0) {
            ESP_LOGE(TAG, "Failed to start voice recorder (non-fatal)");
        }
    }

    behavior_state_set("boot");
    wait_for_behavior_idle(STARTUP_BEHAVIOR_TIMEOUT_MS);
    behavior_state_set_text(cloud_ready ? "Ready!" : "BLE Ready", 0);
    log_heap_state("after_ui_init");
    if (cloud_ready) {
        ws_client_start();
        log_heap_state("after_ws_start");
    }
    run_camera_boot_diag();
    log_heap_state("after_camera_diag");
    /* Note: hal_display_ui_init() already sets "Ready" text and starts default animation.
     * Don't call display_update here as it would override the startup UI state. */
    ESP_LOGI(TAG, "WatcheRobot ready (cloud=%s, ble=%s)", cloud_ready ? "online" : "offline",
             ble_service_is_connected() ? "connected" : "advertising");

    /* 10. Mark OTA partition valid (prevent rollback after successful boot) */
    ota_service_mark_valid();

    /* Main loop */
    esp_task_wdt_add(NULL);
    while (1) {
        esp_task_wdt_reset();
        ws_tts_timeout_check();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
