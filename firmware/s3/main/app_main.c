#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"

#include "bsp_watcher.h"
#include "voice_service.h"
#include "display_ui.h"
#include "wifi_manager.h"
#include "ws_client.h"
#include "ws_handlers.h"
#include "ws_router.h"
#include "discovery_client.h"
#include "hal_display.h"
#include "hal_servo.h"
#include "boot_anim.h"
#include "anim_storage.h"
#include "ota_service.h"
#include "sensecap-watcher.h"

#define TAG "MAIN"

/* Physical restart: click count to trigger reboot */
#define RESTART_CLICK_COUNT  5

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
    int progress = 45 + (types_done * 45) / types_total;
    boot_anim_set_progress(progress);
    boot_anim_set_text(emoji_type_name(type));
}

/* ------------------------------------------------------------------ */
/* Main Application                                                   */
/* ------------------------------------------------------------------ */

void app_main(void)
{
    ESP_LOGI(TAG, "WatcheRobot S3 v2.0 starting");

    /* 1. Minimal display init for boot animation */
    if (hal_display_minimal_init() != 0) {
        ESP_LOGE(TAG, "Failed to initialize display");
        return;
    }

    /* 2. Show boot animation */
    boot_anim_init();
    boot_anim_set_text("Initializing...");

    /* 3. Servo HAL init (GPIO 19/20 LEDC PWM, Phase 2 implementation) */
    boot_anim_set_progress(10);
    boot_anim_set_text("Servo...");
    hal_servo_init();

    /* 4. Voice recorder: init only (do NOT start yet) */
    boot_anim_set_progress(20);
    boot_anim_set_text("Voice...");
    voice_recorder_init();

    /* 5. Register button callbacks */
    bsp_set_btn_long_press_cb(on_button_long_press);
    bsp_set_btn_long_release_cb(on_button_long_release);
    bsp_set_btn_multi_click_cb(RESTART_CLICK_COUNT, on_button_multi_click_restart);

    /* 6. WiFi (skip for animation testing) */
    boot_anim_set_progress(25);
    boot_anim_set_text("WiFi (skipped)...");
    ESP_LOGW(TAG, "WiFi skipped for animation testing");
    boot_anim_set_progress(35);

    /* 7. Service discovery (skip for animation testing) */
    boot_anim_set_text("Discovery (skipped)...");
    ESP_LOGW(TAG, "Discovery skipped for animation testing");
    boot_anim_set_progress(40);

    /* 8. SPIFFS + emoji loading (45% -> 90%) */
    boot_anim_set_progress(45);
    boot_anim_set_text("Loading...");
    if (emoji_spiffs_init() == 0) {
        emoji_load_all_images_with_cb(on_emoji_type_loaded);
    } else {
        ESP_LOGW(TAG, "SPIFFS init failed (emoji disabled)");
        boot_anim_set_progress(90);
    }

    /* 9. Start voice recorder */
    if (voice_recorder_start() != 0) {
        ESP_LOGE(TAG, "Failed to start voice recorder (non-fatal)");
    }

    /* 10. WebSocket client (skip for animation testing) */
    boot_anim_set_progress(92);
    boot_anim_set_text("WS (skipped)...");
    ESP_LOGW(TAG, "WebSocket skipped for animation testing");

    /* 11. Ready! */
    boot_anim_set_progress(100);
    boot_anim_set_text("Ready!");
    vTaskDelay(pdMS_TO_TICKS(500));
    boot_anim_finish();
    hal_display_ui_init();
    display_update("Ready", "happy", 0, NULL);
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
