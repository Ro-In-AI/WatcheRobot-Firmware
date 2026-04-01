#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
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
#define CLOUD_DISCOVERY_TIMEOUT_MS 5000
#define CLOUD_RETRY_DELAY_MS 2000
#define CLOUD_PROTOCOL_RETRY_DELAY_MS 5000
#define CAMERA_DIAG_MIN_INTERNAL_FREE_BYTES (48U * 1024U)
#define CAMERA_DIAG_MIN_INTERNAL_LARGEST_BYTES (16U * 1024U)
#ifdef CONFIG_WATCHER_ANIM_FPS
#define BOOT_ANIM_INTERVAL_MS (1000U / CONFIG_WATCHER_ANIM_FPS)
#else
#define BOOT_ANIM_INTERVAL_MS 100U
#endif

typedef enum {
    TRANSPORT_BLE_ACTIVE = 0,
    TRANSPORT_BLE_IDLE_NO_CREDENTIALS,
    TRANSPORT_BLE_IDLE_WIFI_STARTING,
    TRANSPORT_BLE_IDLE_WIFI_CONNECTING,
    TRANSPORT_BLE_IDLE_DISCOVERING,
    TRANSPORT_BLE_IDLE_WS_CONNECTING,
    TRANSPORT_BLE_IDLE_CLOUD_READY,
    TRANSPORT_BLE_IDLE_CLOUD_SUSPENDED,
} transport_state_t;

typedef enum {
    IDLE_HINT_READY = 0,
    IDLE_HINT_BLE_READY,
    IDLE_HINT_BLE_NO_WIFI,
} idle_hint_mode_t;

typedef struct {
    uint32_t generation;
    int status;
    server_info_t info;
} discovery_result_t;

static bool s_waiting_for_wifi_provision = false;
static bool s_boot_completed = false;
static bool s_cloud_runtime_started = false;
static bool s_ws_router_ready = false;
static bool s_ws_stack_ready = false;
static bool s_discovery_initialized = false;
static bool s_last_ble_connected = false;
static transport_state_t s_transport_state = TRANSPORT_BLE_IDLE_NO_CREDENTIALS;
static volatile bool s_discovery_inflight = false;
static uint32_t s_discovery_generation = 0;
static int64_t s_next_cloud_attempt_us = 0;
static QueueHandle_t s_discovery_result_queue = NULL;

static const char *transport_state_to_string(transport_state_t state) {
    switch (state) {
    case TRANSPORT_BLE_ACTIVE:
        return "BLE_ACTIVE";
    case TRANSPORT_BLE_IDLE_NO_CREDENTIALS:
        return "BLE_IDLE_NO_CREDENTIALS";
    case TRANSPORT_BLE_IDLE_WIFI_STARTING:
        return "BLE_IDLE_WIFI_STARTING";
    case TRANSPORT_BLE_IDLE_WIFI_CONNECTING:
        return "BLE_IDLE_WIFI_CONNECTING";
    case TRANSPORT_BLE_IDLE_DISCOVERING:
        return "BLE_IDLE_DISCOVERING";
    case TRANSPORT_BLE_IDLE_WS_CONNECTING:
        return "BLE_IDLE_WS_CONNECTING";
    case TRANSPORT_BLE_IDLE_CLOUD_READY:
        return "BLE_IDLE_CLOUD_READY";
    case TRANSPORT_BLE_IDLE_CLOUD_SUSPENDED:
    default:
        return "BLE_IDLE_CLOUD_SUSPENDED";
    }
}

static void transport_set_state(transport_state_t state, const char *reason) {
    if (state == s_transport_state) {
        return;
    }

    ESP_LOGI(TAG,
             "Transport state: %s -> %s (%s)",
             transport_state_to_string(s_transport_state),
             transport_state_to_string(state),
             reason ? reason : "no reason");
    s_transport_state = state;
}

static void transport_schedule_retry(uint32_t delay_ms) {
    s_next_cloud_attempt_us = esp_timer_get_time() + ((int64_t)delay_ms * 1000LL);
}

static bool transport_retry_due(void) {
    return s_next_cloud_attempt_us == 0 || esp_timer_get_time() >= s_next_cloud_attempt_us;
}

#if CONFIG_WATCHER_CAMERA_BOOT_DIAG
static bool has_internal_heap_headroom(size_t min_free_bytes, size_t min_largest_block_bytes) {
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    return free_internal >= min_free_bytes && largest_internal >= min_largest_block_bytes;
}
#endif

static void on_wifi_status_changed(wifi_status_t status, const char *ssid, const char *ip_addr) {
    switch (status) {
    case WIFI_STATUS_CONNECTED:
        ESP_LOGI(TAG, "WiFi connected: ssid=%s ip=%s", ssid ? ssid : "<unknown>",
                 ip_addr ? ip_addr : "<no-ip>");
        if (!s_boot_completed && s_waiting_for_wifi_provision) {
            boot_anim_set_text("WiFi Connected");
        }
        break;

    case WIFI_STATUS_CONNECTING:
        ESP_LOGI(TAG, "WiFi connecting: ssid=%s", ssid ? ssid : "<unknown>");
        if (!s_boot_completed && s_waiting_for_wifi_provision) {
            boot_anim_set_text("Connecting WiFi...");
        }
        break;

    case WIFI_STATUS_DISCONNECTED:
        ESP_LOGW(TAG, "WiFi connect failed or disconnected: ssid=%s", ssid ? ssid : "<unknown>");
        if (!s_boot_completed && s_waiting_for_wifi_provision) {
            boot_anim_set_text("WiFi Failed, Retry");
        }
        break;

    case WIFI_STATUS_UNCONFIGURED:
    default:
        ESP_LOGI(TAG, "WiFi unconfigured");
        if (!s_boot_completed && s_waiting_for_wifi_provision) {
            boot_anim_set_text("Open APP Set WiFi");
        }
        break;
    }
}

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
    if (s_transport_state == TRANSPORT_BLE_IDLE_CLOUD_READY) {
        return IDLE_HINT_READY;
    }

    if (ble_service_is_connected() || wifi_has_credentials() != 1) {
        return IDLE_HINT_BLE_NO_WIFI;
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

static void ensure_cloud_runtime_started(void) {
    if (!s_boot_completed || s_cloud_runtime_started) {
        return;
    }

    if (hal_display_input_init() != 0) {
        ESP_LOGW(TAG, "Delayed display input initialization failed");
    }

    if (hal_display_has_knob_input()) {
        bsp_set_btn_multi_click_cb(RESTART_CLICK_COUNT, on_button_multi_click_restart);
    } else {
        ESP_LOGW(TAG, "Skipping %d-click restart callback because knob input is unavailable", RESTART_CLICK_COUNT);
    }

    voice_recorder_init();
    if (voice_recorder_start() != 0) {
        ESP_LOGE(TAG, "Failed to start voice recorder (non-fatal)");
    }

    s_cloud_runtime_started = true;
}

static void transport_stop_ws(const char *reason) {
    if (ws_client_is_started() || ws_client_is_connected() || ws_client_is_session_ready()) {
        ESP_LOGI(TAG, "Stopping WebSocket transport (%s)", reason ? reason : "no reason");
        ws_client_stop();
    }
}

static void transport_discovery_task(void *arg) {
    discovery_result_t result = {0};
    uint32_t *generation = (uint32_t *)arg;

    result.generation = generation ? *generation : 0;
    result.status = discovery_start_with_timeout(&result.info, CLOUD_DISCOVERY_TIMEOUT_MS);

    if (s_discovery_result_queue != NULL) {
        (void)xQueueOverwrite(s_discovery_result_queue, &result);
    }

    s_discovery_inflight = false;
    free(generation);
    vTaskDelete(NULL);
}

static int transport_launch_discovery(void) {
    BaseType_t task_ret;
    uint32_t *generation;

    if (s_discovery_inflight) {
        return 0;
    }

    if (s_discovery_result_queue == NULL) {
        s_discovery_result_queue = xQueueCreate(1, sizeof(discovery_result_t));
        if (s_discovery_result_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create discovery result queue");
            return -1;
        }
    }

    generation = (uint32_t *)malloc(sizeof(uint32_t));
    if (generation == NULL) {
        ESP_LOGE(TAG, "Failed to allocate discovery generation");
        return -1;
    }

    s_discovery_generation += 1U;
    *generation = s_discovery_generation;
    s_discovery_inflight = true;

    task_ret = xTaskCreate(transport_discovery_task,
                           "cloud_discovery",
                           4096,
                           generation,
                           5,
                           NULL);
    if (task_ret != pdPASS) {
        s_discovery_inflight = false;
        free(generation);
        ESP_LOGE(TAG, "Failed to create discovery task");
        return -1;
    }

    ESP_LOGI(TAG, "Started discovery generation %lu", (unsigned long)*generation);
    transport_set_state(TRANSPORT_BLE_IDLE_DISCOVERING, "discovery task launched");
    return 0;
}

static void transport_cancel_discovery(const char *reason) {
    if (!s_discovery_inflight) {
        return;
    }

    s_discovery_generation += 1U;
    ESP_LOGI(TAG, "Discovery invalidated (%s)", reason ? reason : "no reason");
}

static int transport_prepare_ws_client(const char *ws_url) {
    if (ws_url == NULL || ws_url[0] == '\0') {
        return -1;
    }

    if (!s_ws_router_ready) {
        ws_router_t router;

        ws_handlers_init();
        router = ws_handlers_get_router();
        ws_router_init(&router);
        s_ws_router_ready = true;
    }

    if (!s_ws_stack_ready || strcmp(ws_client_get_server_url(), ws_url) != 0) {
        if (s_ws_stack_ready) {
            transport_stop_ws("ws url refresh");
            ws_client_deinit();
            s_ws_stack_ready = false;
        }

        if (ws_client_set_server_url(ws_url) != 0) {
            ESP_LOGE(TAG, "Failed to set WebSocket URL: %s", ws_url);
            return -1;
        }

        if (ws_client_init() != 0) {
            ESP_LOGE(TAG, "Failed to initialize WebSocket client");
            return -1;
        }

        s_ws_stack_ready = true;
    }

    return 0;
}

static void transport_begin_wifi_resume(const char *reason) {
    if (wifi_has_credentials() != 1) {
        s_waiting_for_wifi_provision = true;
        transport_stop_ws("missing credentials");
        transport_set_state(TRANSPORT_BLE_IDLE_NO_CREDENTIALS, reason);
        return;
    }

    s_waiting_for_wifi_provision = false;

    if (wifi_is_connected() == 1) {
        return;
    }

    if (wifi_is_connect_requested() == 1) {
        transport_set_state(wifi_sta_is_started() == 1
                                ? TRANSPORT_BLE_IDLE_WIFI_CONNECTING
                                : TRANSPORT_BLE_IDLE_WIFI_STARTING,
                            reason);
        return;
    }

    if (wifi_resume_background() == 0) {
        transport_set_state(wifi_sta_is_started() == 1
                                ? TRANSPORT_BLE_IDLE_WIFI_CONNECTING
                                : TRANSPORT_BLE_IDLE_WIFI_STARTING,
                            reason);
        return;
    }

    transport_schedule_retry(CLOUD_RETRY_DELAY_MS);
    transport_set_state(TRANSPORT_BLE_IDLE_CLOUD_SUSPENDED, "wifi resume failed");
}

static void transport_handle_discovery_results(bool ble_connected) {
    discovery_result_t result;

    if (s_discovery_result_queue == NULL) {
        return;
    }

    while (xQueueReceive(s_discovery_result_queue, &result, 0) == pdTRUE) {
        char *ws_url = NULL;

        if (result.generation != s_discovery_generation) {
            ESP_LOGI(TAG,
                     "Ignoring stale discovery result generation %lu (current %lu)",
                     (unsigned long)result.generation,
                     (unsigned long)s_discovery_generation);
            continue;
        }

        if (ble_connected) {
            ESP_LOGI(TAG, "Ignoring discovery result because BLE is active");
            continue;
        }

        if (result.status != 0) {
            ESP_LOGW(TAG, "Discovery failed, retrying after %u ms", CLOUD_RETRY_DELAY_MS);
            transport_schedule_retry(CLOUD_RETRY_DELAY_MS);
            transport_set_state(TRANSPORT_BLE_IDLE_CLOUD_SUSPENDED, "discovery failed");
            continue;
        }

        if (result.info.protocol_version[0] == '\0' ||
            strcmp(result.info.protocol_version, WATCHER_PROTOCOL_VERSION) != 0) {
            ESP_LOGE(TAG,
                     "Protocol mismatch: server=%s expected=%s",
                     result.info.protocol_version[0] != '\0' ? result.info.protocol_version : "<missing>",
                     WATCHER_PROTOCOL_VERSION);
            transport_schedule_retry(CLOUD_PROTOCOL_RETRY_DELAY_MS);
            transport_set_state(TRANSPORT_BLE_IDLE_CLOUD_SUSPENDED, "protocol mismatch");
            continue;
        }

        ESP_LOGI(TAG,
                 "Discovery ready: %s:%u protocol=%s",
                 result.info.ip,
                 result.info.port,
                 result.info.protocol_version);

        ws_url = discovery_get_ws_url(&result.info);
        if (ws_url == NULL) {
            ESP_LOGE(TAG, "Failed to build WebSocket URL from discovery result");
            transport_schedule_retry(CLOUD_RETRY_DELAY_MS);
            transport_set_state(TRANSPORT_BLE_IDLE_CLOUD_SUSPENDED, "ws url build failed");
            continue;
        }

        if (transport_prepare_ws_client(ws_url) != 0) {
            free(ws_url);
            transport_schedule_retry(CLOUD_RETRY_DELAY_MS);
            transport_set_state(TRANSPORT_BLE_IDLE_CLOUD_SUSPENDED, "ws prepare failed");
            continue;
        }

        free(ws_url);
        ensure_cloud_runtime_started();

        if (ws_client_start() != 0) {
            transport_stop_ws("ws start failed");
            transport_schedule_retry(CLOUD_RETRY_DELAY_MS);
            transport_set_state(TRANSPORT_BLE_IDLE_CLOUD_SUSPENDED, "ws start failed");
            continue;
        }

        transport_schedule_retry(0);
        log_heap_state("after_ws_start");
        transport_set_state(TRANSPORT_BLE_IDLE_WS_CONNECTING, "ws start requested");
    }
}

static void transport_suspend_for_ble(void) {
    ESP_LOGI(TAG, "BLE connected, pausing WiFi/WS background activity");
    transport_cancel_discovery("ble connected");
    transport_stop_ws("ble connected");
    wifi_suspend_for_ble();
    transport_schedule_retry(0);
    transport_set_state(TRANSPORT_BLE_ACTIVE, "ble connected");
}

static void transport_coordinator_tick(void) {
    bool ble_connected = ble_service_is_connected();

    transport_handle_discovery_results(ble_connected);

    if (ble_connected != s_last_ble_connected) {
        s_last_ble_connected = ble_connected;

        if (ble_connected) {
            transport_suspend_for_ble();
            return;
        }

        ESP_LOGI(TAG, "BLE disconnected, allowing WiFi/WS recovery");
        transport_set_state(TRANSPORT_BLE_IDLE_CLOUD_SUSPENDED, "ble disconnected");
    }

    if (ble_connected) {
        transport_set_state(TRANSPORT_BLE_ACTIVE, "ble still connected");
        return;
    }

    if (wifi_has_credentials() != 1) {
        s_waiting_for_wifi_provision = true;
        transport_stop_ws("credentials cleared");
        transport_set_state(TRANSPORT_BLE_IDLE_NO_CREDENTIALS, "no credentials");
        return;
    }

    if (wifi_is_connected() != 1) {
        transport_stop_ws("wifi not connected");

        if (transport_retry_due()) {
            transport_begin_wifi_resume("wifi recovery");
        } else {
            transport_set_state(TRANSPORT_BLE_IDLE_CLOUD_SUSPENDED, "waiting wifi retry");
        }
        return;
    }

    s_waiting_for_wifi_provision = false;

    if (!s_discovery_initialized) {
        if (discovery_init() == 0) {
            s_discovery_initialized = true;
        } else {
            ESP_LOGW(TAG, "Discovery init failed, retrying later");
            transport_schedule_retry(CLOUD_RETRY_DELAY_MS);
            transport_set_state(TRANSPORT_BLE_IDLE_CLOUD_SUSPENDED, "discovery init failed");
            return;
        }
    }

    if (ws_client_is_session_ready()) {
        transport_set_state(TRANSPORT_BLE_IDLE_CLOUD_READY, "ws session ready");
        return;
    }

    if (ws_client_is_connected() || ws_client_is_started()) {
        transport_set_state(TRANSPORT_BLE_IDLE_WS_CONNECTING, "ws connecting");
        return;
    }

    if (s_discovery_inflight) {
        transport_set_state(TRANSPORT_BLE_IDLE_DISCOVERING, "discovery in flight");
        return;
    }

    if (!transport_retry_due()) {
        transport_set_state(TRANSPORT_BLE_IDLE_CLOUD_SUSPENDED, "waiting cloud retry");
        return;
    }

    if (transport_launch_discovery() != 0) {
        transport_schedule_retry(CLOUD_RETRY_DELAY_MS);
        transport_set_state(TRANSPORT_BLE_IDLE_CLOUD_SUSPENDED, "discovery launch failed");
    }
}

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

    if (control_ingress_init() != ESP_OK) {
        ESP_LOGE(TAG, "Control ingress init failed");
        return;
    }

    /* 5.5 BLE control + provisioning */
    boot_anim_set_progress(35);
    boot_anim_set_text("BLE...");
    {
        esp_err_t ble_ret = ble_service_init();
        if (ble_ret == ESP_OK) {
            ble_ret = ble_service_start_advertising();
            if (ble_ret != ESP_OK) {
                ESP_LOGW(TAG, "BLE advertising start failed: %s", esp_err_to_name(ble_ret));
            }
        } else if (ble_ret != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "BLE init failed: %s", esp_err_to_name(ble_ret));
        }
    }
    log_heap_state("after_ble_init");

    /* 6. WiFi manager only; cloud link is coordinated after boot. */
    boot_anim_set_progress(40);
    boot_anim_set_text("WiFi...");
    wifi_init();
    wifi_register_status_callback(on_wifi_status_changed);
    if (wifi_has_credentials() == 1 && !ble_service_is_connected()) {
        if (wifi_resume_background() == 0) {
            ESP_LOGI(TAG, "Boot WiFi resume accepted; cloud coordinator will continue after boot");
        } else {
            s_waiting_for_wifi_provision = true;
            boot_anim_set_text("BLE Ready");
            ESP_LOGI(TAG, "Unable to resume WiFi at boot, continuing with BLE-only local control");
        }
    } else {
        s_waiting_for_wifi_provision = true;
        boot_anim_set_text("BLE Ready");
        ESP_LOGI(TAG, "BLE local control is ready without immediate cloud bring-up");
    }
    boot_anim_set_progress(55);
    log_heap_state("after_wifi_ready");

    /* 7. Ready for UI; cloud transport is resumed by the coordinator loop. */
    boot_anim_set_progress(100);
    boot_anim_set_text("BLE Ready");
    vTaskDelay(pdMS_TO_TICKS(500));
    boot_anim_finish();
    s_boot_completed = true;
    s_waiting_for_wifi_provision = false;

    log_heap_state("before_ui_init");
    hal_display_ui_init();
    behavior_state_set("boot");
    wait_for_behavior_idle(STARTUP_BEHAVIOR_TIMEOUT_MS);
    behavior_state_set_text_style("BLE Ready", 0, false);
    apply_idle_hint_if_needed();
    log_heap_state("after_ui_init");

    run_camera_boot_diag();
    log_heap_state("after_camera_diag");
    ESP_LOGI(TAG, "WatcheRobot ready (transport=%s, ble=%s)",
             transport_state_to_string(s_transport_state),
             ble_service_is_connected() ? "connected" : "advertising");

    /* 10. Mark OTA partition valid (prevent rollback after successful boot) */
    ota_service_mark_valid();

    /* Main loop */
    esp_task_wdt_add(NULL);
    while (1) {
        esp_task_wdt_reset();
        transport_coordinator_tick();
        apply_idle_hint_if_needed();
        ws_tts_timeout_check();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
