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
#include "control_ingress.h"
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
#define BOOT_DISCOVERY_TIMEOUT_MS 5000
#define CAMERA_DIAG_MIN_INTERNAL_FREE_BYTES (48U * 1024U)
#define CAMERA_DIAG_MIN_INTERNAL_LARGEST_BYTES (16U * 1024U)
#ifdef CONFIG_WATCHER_ANIM_FPS
#define BOOT_ANIM_INTERVAL_MS (1000U / CONFIG_WATCHER_ANIM_FPS)
#else
#define BOOT_ANIM_INTERVAL_MS 100U
#endif

static bool s_waiting_for_wifi_provision = false;
static bool s_ble_only_mode = false;
static bool s_wifi_paused_for_ble = false;
static bool s_ws_paused_for_ble = false;
static bool s_cloud_session_configured = false;

typedef enum {
    IDLE_HINT_READY = 0,
    IDLE_HINT_BLE_READY,
    IDLE_HINT_BLE_NO_WIFI,
} idle_hint_mode_t;

static bool has_internal_heap_headroom(size_t min_free_bytes, size_t min_largest_block_bytes) {
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    return free_internal >= min_free_bytes && largest_internal >= min_largest_block_bytes;
}

static void on_wifi_status_changed(wifi_status_t status, const char *ssid, const char *ip_addr) {
    switch (status) {
    case WIFI_STATUS_CONNECTED:
        ESP_LOGI(TAG, "WiFi connected: ssid=%s ip=%s", ssid ? ssid : "<unknown>",
                 ip_addr ? ip_addr : "<no-ip>");
        if (s_wifi_paused_for_ble) {
            break;
        }
        if (s_waiting_for_wifi_provision) {
            boot_anim_set_text("WiFi Connected");
        }
        break;

    case WIFI_STATUS_CONNECTING:
        ESP_LOGI(TAG, "WiFi connecting: ssid=%s", ssid ? ssid : "<unknown>");
        if (s_wifi_paused_for_ble) {
            break;
        }
        if (s_waiting_for_wifi_provision) {
            boot_anim_set_text("Connecting WiFi...");
        }
        break;

    case WIFI_STATUS_DISCONNECTED:
        ESP_LOGW(TAG, "WiFi connect failed or disconnected: ssid=%s", ssid ? ssid : "<unknown>");
        if (s_wifi_paused_for_ble) {
            break;
        }
        if (s_waiting_for_wifi_provision) {
            boot_anim_set_text("WiFi Failed, Retry");
        }
        break;

    case WIFI_STATUS_UNCONFIGURED:
    default:
        ESP_LOGI(TAG, "WiFi unconfigured");
        if (s_wifi_paused_for_ble) {
            break;
        }
        if (s_waiting_for_wifi_provision) {
            boot_anim_set_text("Open APP Set WiFi");
        }
        break;
    }
}

static void handle_ble_transport_gate(bool ble_connected) {
    static bool last_ble_connected = false;

    if (ble_connected == last_ble_connected) {
        if (!ble_connected &&
            s_ws_paused_for_ble &&
            s_cloud_session_configured &&
            wifi_is_connected() == 1 &&
            !ws_client_is_connected()) {
            ESP_LOGI(TAG, "BLE released, resuming WebSocket session");
            ws_client_start();
            s_ws_paused_for_ble = false;
        }
        return;
    }

    last_ble_connected = ble_connected;

    if (ble_connected) {
        ESP_LOGI(TAG, "BLE connected, pausing WiFi/WS background activity");
        s_wifi_paused_for_ble = true;
        if (ws_client_is_connected() || ws_client_is_session_ready()) {
            ws_client_stop();
            s_ws_paused_for_ble = s_cloud_session_configured;
        }
        wifi_disconnect();
        return;
    }

    ESP_LOGI(TAG, "BLE disconnected, resuming WiFi/WS background activity");
    s_wifi_paused_for_ble = false;
    if (wifi_has_credentials() == 1) {
        wifi_connect_async();
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
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (!has_internal_heap_headroom(CAMERA_DIAG_MIN_INTERNAL_FREE_BYTES,
                                    CAMERA_DIAG_MIN_INTERNAL_LARGEST_BYTES)) {
        ESP_LOGW(TAG,
                 "Skipping camera boot diagnostic due to low internal heap: free=%u largest=%u "
                 "(need >=%u / >=%u)",
                 (unsigned)free_internal,
                 (unsigned)largest_internal,
                 (unsigned)CAMERA_DIAG_MIN_INTERNAL_FREE_BYTES,
                 (unsigned)CAMERA_DIAG_MIN_INTERNAL_LARGEST_BYTES);
        return;
    }

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

    if (wifi_connect_async() == 0) {
        return 0;
    }

    ESP_LOGW(TAG, "Unable to start WiFi background connect, continuing with BLE-only local control");
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

static idle_hint_mode_t get_idle_hint_mode(void) {
    if (ble_service_is_connected() && wifi_is_connected() != 1) {
        return IDLE_HINT_BLE_NO_WIFI;
    }

    if (s_cloud_session_configured && wifi_is_connected() == 1) {
        return IDLE_HINT_READY;
    }

    return IDLE_HINT_BLE_READY;
}

static void apply_idle_hint_if_needed(void) {
    static idle_hint_mode_t s_last_applied_hint = IDLE_HINT_READY;
    static bool s_hint_initialized = false;
    idle_hint_mode_t desired_hint = get_idle_hint_mode();

    if (behavior_state_is_busy() || behavior_state_is_action_active()) {
        return;
    }

    if (s_hint_initialized && desired_hint == s_last_applied_hint) {
        return;
    }

    switch (desired_hint) {
    case IDLE_HINT_BLE_NO_WIFI:
        (void)behavior_state_set_with_text_style("standby", "BLE Ready but no WiFi", 24, true);
        break;

    case IDLE_HINT_READY:
        (void)behavior_state_set_with_text_style("standby", "Ready!", 0, false);
        break;

    case IDLE_HINT_BLE_READY:
    default:
        (void)behavior_state_set_with_text_style("standby", "BLE Ready", 0, false);
        break;
    }

    s_last_applied_hint = desired_hint;
    s_hint_initialized = true;
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
        int boot_frame_count = emoji_load_type(EMOJI_ANIM_BOOT);
        if (boot_frame_count > 0) {
            boot_anim_start_intro(EMOJI_ANIM_BOOT, boot_frame_count, BOOT_ANIM_INTERVAL_MS);
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

    esp_err_t ingress_ret = control_ingress_init();
    if (ingress_ret != ESP_OK) {
        ESP_LOGE(TAG, "Control ingress init failed: %s", esp_err_to_name(ingress_ret));
        return;
    }

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
    log_heap_state("after_ble_init");

    /* 6. WiFi */
    boot_anim_set_progress(40);
    boot_anim_set_text("WiFi...");
    wifi_init();
    wifi_register_status_callback(on_wifi_status_changed);
    if (ensure_boot_wifi_connection() != 0) {
        s_waiting_for_wifi_provision = true;
        boot_anim_set_text("BLE Ready");
        ESP_LOGI(TAG, "BLE local control is ready without WiFi/WebSocket");
    } else {
        ESP_LOGI(TAG, "WiFi connect is running in background; BLE local control remains available");
    }
    boot_anim_set_progress(55);
    log_heap_state("after_wifi_ready");

    /* 7. Service discovery */
    if (wifi_is_connected() == 1) {
        boot_anim_set_text("Discovering...");
        discovery_init();
        server_info_t server_info = {0};
        if (discovery_start_with_timeout(&server_info, BOOT_DISCOVERY_TIMEOUT_MS) == 0) {
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
                s_cloud_session_configured = true;
            }
        } else {
            ESP_LOGW(TAG, "Discovery failed within %d ms, continuing in BLE-only mode", BOOT_DISCOVERY_TIMEOUT_MS);
        }
    } else {
        ESP_LOGI(TAG, "Skipping discovery until WiFi is connected; BLE local control continues immediately");
    }
    log_heap_state("after_discovery");

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
    s_waiting_for_wifi_provision = false;
    log_heap_state("before_ui_init");
    hal_display_ui_init();
    if (cloud_ready) {
        if (hal_display_input_init() != 0) {
            ESP_LOGW(TAG, "Delayed display input initialization failed");
        }

        /* Register encoder callbacks only after delayed input devices are attached. */
        if (hal_display_has_knob_input()) {
            bsp_set_btn_multi_click_cb(RESTART_CLICK_COUNT, on_button_multi_click_restart);
        } else {
            ESP_LOGW(TAG, "Skipping %d-click restart callback because knob input is unavailable", RESTART_CLICK_COUNT);
        }

        voice_recorder_init();
        if (voice_recorder_start() != 0) {
            ESP_LOGE(TAG, "Failed to start voice recorder (non-fatal)");
        }
    }

    behavior_state_set("boot");
    wait_for_behavior_idle(STARTUP_BEHAVIOR_TIMEOUT_MS);
    behavior_state_set_text_style(cloud_ready ? "Ready!" : "BLE Ready", 0, false);
    apply_idle_hint_if_needed();
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
        handle_ble_transport_gate(ble_service_is_connected());
        apply_idle_hint_if_needed();
        ws_tts_timeout_check();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
