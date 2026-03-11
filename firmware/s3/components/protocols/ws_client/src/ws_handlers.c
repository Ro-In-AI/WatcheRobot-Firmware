/**
 * @file ws_handlers.c
 * @brief WebSocket message handlers implementation (Protocol v2.1)
 */

#include "ws_handlers.h"
#include "ws_client.h"
#include "hal_servo.h"
#include "display_ui.h"
#include "esp_system.h"
#include "esp_log.h"
#include <string.h>

#define TAG "WS_HANDLERS"

/* Default servo movement duration (ms) */
#define SERVO_DEFAULT_DURATION_MS 100

/* ------------------------------------------------------------------ */
/* Helper: Parse status data to determine emoji                       */
/* ------------------------------------------------------------------ */

const char* ws_status_data_to_emoji(const char *data)
{
    if (!data || data[0] == '\0') {
        return NULL;  /* No change */
    }

    /* Check for status indicators in data string */

    /* AI processing states - show analyzing animation */
    if (strstr(data, "processing") != NULL) {
        return "analyzing";
    }
    if (strstr(data, "thinking") != NULL) {
        return "analyzing";
    }
    if (strstr(data, "[thinking]") != NULL) {
        return "analyzing";
    }

    /* Speaking state - show speaking animation */
    if (strstr(data, "speaking") != NULL) {
        return "speaking";
    }

    /* Idle/done states - return to standby */
    if (strstr(data, "idle") != NULL) {
        return "standby";
    }
    if (strstr(data, "done") != NULL) {
        return "happy";
    }

    /* Error states */
    if (strstr(data, "error") != NULL) {
        return "sad";
    }

    /* Servo animation status - no change */
    if (strstr(data, "舵机动画") != NULL) {
        return NULL;
    }

    /* Default: no change */
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Handler: Servo Command (v2.1 format)                               */
/* ------------------------------------------------------------------ */

void on_servo_handler(const ws_servo_cmd_t *cmd)
{
    if (!cmd) {
        return;
    }

    /* Use ESP_LOGD to avoid flooding logs with high-frequency servo commands */
    ESP_LOGI(TAG, "Servo command: id=%s, angle=%d, time=%d",
             cmd->id, cmd->angle, cmd->time_ms);

    /* Send single servo command via HAL */
    hal_servo_send_cmd(cmd->id, cmd->angle, cmd->time_ms);
}

/* ------------------------------------------------------------------ */
/* Handler: Display Command                                           */
/* ------------------------------------------------------------------ */

void on_display_handler(const ws_display_cmd_t *cmd)
{
    if (!cmd) {
        return;
    }

    /* Get emoji, use "normal" if empty */
    const char *emoji = cmd->emoji;
    if (!emoji || emoji[0] == '\0') {
        emoji = "normal";
    }

    /* Update display */
    display_update(cmd->text, emoji, cmd->size, NULL);
}

/* ------------------------------------------------------------------ */
/* Handler: Status Command (v2.0)                                     */
/* ------------------------------------------------------------------ */

void on_status_handler(const ws_status_cmd_t *cmd)
{
    if (!cmd) {
        return;
    }

    ESP_LOGI(TAG, "Status: %s", cmd->data);

    /* Map status data to emoji */
    const char *emoji = ws_status_data_to_emoji(cmd->data);

    /* Update display with status data and appropriate emoji */
    if (emoji) {
        display_update(cmd->data, emoji, 0, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* Handler: Capture Command (MVP: Not Implemented)                    */
/* ------------------------------------------------------------------ */

void on_capture_handler(const ws_capture_cmd_t *cmd)
{
    /* MVP: Camera capture not implemented */
    (void)cmd;
}

/* ------------------------------------------------------------------ */
/* Handler: Reboot Command                                            */
/* ------------------------------------------------------------------ */

void on_reboot_handler(void)
{
    ESP_LOGI(TAG, "Reboot command received");
    esp_restart();
}

/* ------------------------------------------------------------------ */
/* Handler: ASR Result (v2.0)                                         */
/* ------------------------------------------------------------------ */

void on_asr_result_handler(const ws_asr_result_cmd_t *cmd)
{
    if (!cmd) {
        return;
    }

    ESP_LOGI(TAG, "ASR result: %s", cmd->text);

    /* Handle empty ASR result - show placeholder text */
    if (cmd->text[0] == 0) {
        display_update("Listening...", "listening", 0, NULL);
    } else {
        /* Display recognized text with analyzing animation */
        display_update(cmd->text, "analyzing", 0, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* Handler: Bot Reply (v2.0)                                          */
/* ------------------------------------------------------------------ */

void on_bot_reply_handler(const ws_bot_reply_cmd_t *cmd)
{
    if (!cmd) {
        return;
    }

    ESP_LOGI(TAG, "Bot reply: %s", cmd->text);

    /* Optional: Display bot reply text */
    /* The TTS audio will follow, so we don't change animation here */
}

/* ------------------------------------------------------------------ */
/* Handler: TTS End (v2.0)                                           */
/* ------------------------------------------------------------------ */

void on_tts_end_handler(void)
{
    ESP_LOGI(TAG, "TTS end received");

    /* Complete TTS playback and switch to happy */
    ws_tts_complete();
}

/* ------------------------------------------------------------------ */
/* Handler: Error Message (v2.0)                                      */
/* ------------------------------------------------------------------ */

void on_error_handler(const ws_error_cmd_t *cmd)
{
    if (!cmd) {
        return;
    }

    ESP_LOGE(TAG, "Error (code %d): %s", cmd->code, cmd->message);

    /* Display error state */
    display_update(cmd->message, "sad", 0, NULL);
}

/* ------------------------------------------------------------------ */
/* Convenience: Get Router with All Handlers                          */
/* ------------------------------------------------------------------ */

ws_router_t ws_handlers_get_router(void)
{
    ws_router_t router = {
        .on_servo   = on_servo_handler,
        .on_display = on_display_handler,
        .on_status  = on_status_handler,
        .on_capture = on_capture_handler,
        .on_reboot  = on_reboot_handler,

        /* New handlers - v2.0 */
        .on_asr_result = on_asr_result_handler,
        .on_bot_reply  = on_bot_reply_handler,
        .on_tts_end    = on_tts_end_handler,
        .on_error      = on_error_handler,
    };
    return router;
}
