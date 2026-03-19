/**
 * @file ws_handlers.c
 * @brief WebSocket message handlers implementation (Protocol v2.1)
 */

#include "ws_handlers.h"
#include "camera_service.h"
#include "display_ui.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal_servo.h"
#include "ws_client.h"
#include <string.h>
#include <strings.h>

#define TAG "WS_HANDLERS"

/* Default servo movement duration (ms) */
#define SERVO_DEFAULT_DURATION_MS 100
#define WS_CAPTURE_DEFAULT_FPS 5
#define WS_CAPTURE_MAX_FPS 10
#define WS_CAPTURE_DEFAULT_QUALITY 80

typedef struct {
    SemaphoreHandle_t lock;
    bool callback_registered;
    bool streaming;
    uint16_t current_stream_id;
    uint16_t next_stream_id;
    uint32_t next_seq;
    int target_fps;
    int configured_width;
    int configured_height;
    int configured_quality;
    uint32_t frames_sent;
    uint64_t send_total_us;
    uint64_t send_lock_wait_total_us;
    uint64_t send_sock_total_us;
    char current_command_id[WS_COMMAND_ID_MAX];
} ws_camera_context_t;

static ws_camera_context_t s_camera_ctx = {
    .lock = NULL,
    .callback_registered = false,
    .streaming = false,
    .current_stream_id = 0,
    .next_stream_id = 0,
    .next_seq = 1,
    .target_fps = 0,
    .configured_width = 0,
    .configured_height = 0,
    .configured_quality = WS_CAPTURE_DEFAULT_QUALITY,
    .frames_sent = 0,
    .send_total_us = 0,
    .send_lock_wait_total_us = 0,
    .send_sock_total_us = 0,
    .current_command_id = {0},
};

static esp_err_t ws_camera_ensure_lock(void) {
    if (s_camera_ctx.lock == NULL) {
        s_camera_ctx.lock = xSemaphoreCreateMutex();
        if (s_camera_ctx.lock == NULL) {
            ESP_LOGE(TAG, "camera ws lock create failed");
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

static uint16_t ws_camera_allocate_stream_id_locked(void) {
    s_camera_ctx.next_stream_id++;
    if (s_camera_ctx.next_stream_id == 0) {
        s_camera_ctx.next_stream_id = 1;
    }
    return s_camera_ctx.next_stream_id;
}

static uint16_t ws_camera_begin_transfer(bool streaming, int fps, const char *command_id) {
    uint16_t stream_id = 0;

    if (ws_camera_ensure_lock() != ESP_OK) {
        return 0;
    }

    if (xSemaphoreTake(s_camera_ctx.lock, portMAX_DELAY) == pdTRUE) {
        s_camera_ctx.streaming = streaming;
        s_camera_ctx.target_fps = streaming ? fps : 0;
        s_camera_ctx.current_stream_id = ws_camera_allocate_stream_id_locked();
        s_camera_ctx.next_seq = 1;
        s_camera_ctx.frames_sent = 0;
        s_camera_ctx.send_total_us = 0;
        s_camera_ctx.send_lock_wait_total_us = 0;
        s_camera_ctx.send_sock_total_us = 0;
        stream_id = s_camera_ctx.current_stream_id;
        if (command_id) {
            strncpy(s_camera_ctx.current_command_id, command_id, sizeof(s_camera_ctx.current_command_id) - 1);
            s_camera_ctx.current_command_id[sizeof(s_camera_ctx.current_command_id) - 1] = '\0';
        } else {
            s_camera_ctx.current_command_id[0] = '\0';
        }
        xSemaphoreGive(s_camera_ctx.lock);
    }

    return stream_id;
}

static void ws_camera_finish_one_shot(void) {
    if (ws_camera_ensure_lock() != ESP_OK) {
        return;
    }

    if (xSemaphoreTake(s_camera_ctx.lock, portMAX_DELAY) == pdTRUE) {
        s_camera_ctx.current_stream_id = 0;
        s_camera_ctx.next_seq = 1;
        s_camera_ctx.current_command_id[0] = '\0';
        s_camera_ctx.frames_sent = 0;
        s_camera_ctx.send_total_us = 0;
        s_camera_ctx.send_lock_wait_total_us = 0;
        s_camera_ctx.send_sock_total_us = 0;
        xSemaphoreGive(s_camera_ctx.lock);
    }
}

static void ws_camera_reset_stream(bool keep_config) {
    if (ws_camera_ensure_lock() != ESP_OK) {
        return;
    }

    if (xSemaphoreTake(s_camera_ctx.lock, portMAX_DELAY) == pdTRUE) {
        s_camera_ctx.streaming = false;
        s_camera_ctx.target_fps = 0;
        s_camera_ctx.current_stream_id = 0;
        s_camera_ctx.next_seq = 1;
        s_camera_ctx.current_command_id[0] = '\0';
        if (!keep_config) {
            s_camera_ctx.configured_width = 0;
            s_camera_ctx.configured_height = 0;
            s_camera_ctx.configured_quality = WS_CAPTURE_DEFAULT_QUALITY;
        }
        s_camera_ctx.frames_sent = 0;
        s_camera_ctx.send_total_us = 0;
        s_camera_ctx.send_lock_wait_total_us = 0;
        s_camera_ctx.send_sock_total_us = 0;
        xSemaphoreGive(s_camera_ctx.lock);
    }
}

static void ws_camera_frame_cb(const uint8_t *jpeg, size_t size, uint32_t timestamp_ms, void *ctx) {
    uint32_t seq = 0;
    uint16_t stream_id = 0;
    bool streaming = false;
    bool first_frame = false;
    ws_client_media_send_stats_t send_stats = {0};
    uint32_t frames_sent = 0;
    uint64_t send_total_us = 0;
    uint64_t send_lock_wait_total_us = 0;
    uint64_t send_sock_total_us = 0;
    int send_ret = -1;

    (void)timestamp_ms;
    (void)ctx;

    if (!jpeg || size == 0) {
        return;
    }

    if (ws_camera_ensure_lock() != ESP_OK) {
        return;
    }

    if (xSemaphoreTake(s_camera_ctx.lock, portMAX_DELAY) == pdTRUE) {
        streaming = s_camera_ctx.streaming;
        stream_id = s_camera_ctx.current_stream_id;
        seq = s_camera_ctx.next_seq++;
        first_frame = (seq == 1);
        xSemaphoreGive(s_camera_ctx.lock);
    }

    if (stream_id == 0) {
        ESP_LOGW(TAG, "camera frame dropped: stream not initialized");
        return;
    }

    if (streaming) {
        send_ret = ws_send_video_frame(jpeg, size, stream_id, seq, first_frame);
        ws_client_get_media_send_stats(&send_stats);
        if (send_ret < 0) {
            ESP_LOGW(TAG, "video frame upload failed: stream=%u seq=%lu size=%u",
                     (unsigned int)stream_id,
                     (unsigned long)seq,
                     (unsigned int)size);
        } else if (send_stats.valid && xSemaphoreTake(s_camera_ctx.lock, portMAX_DELAY) == pdTRUE) {
            s_camera_ctx.frames_sent++;
            s_camera_ctx.send_total_us += send_stats.total_us;
            s_camera_ctx.send_lock_wait_total_us += send_stats.lock_wait_us;
            s_camera_ctx.send_sock_total_us += send_stats.send_us;
            frames_sent = s_camera_ctx.frames_sent;
            send_total_us = s_camera_ctx.send_total_us;
            send_lock_wait_total_us = s_camera_ctx.send_lock_wait_total_us;
            send_sock_total_us = s_camera_ctx.send_sock_total_us;
            xSemaphoreGive(s_camera_ctx.lock);
            if ((frames_sent % 30U) == 0U) {
                ESP_LOGI(TAG,
                         "ws media timing stream=%u latest_us{total=%lu lock=%lu send=%lu payload=%u packet=%u} "
                         "avg_us{total=%lu lock=%lu send=%lu} frames=%lu",
                         (unsigned int)stream_id,
                         (unsigned long)send_stats.total_us,
                         (unsigned long)send_stats.lock_wait_us,
                         (unsigned long)send_stats.send_us,
                         (unsigned int)send_stats.payload_len,
                         (unsigned int)send_stats.packet_len,
                         (unsigned long)(send_total_us / frames_sent),
                         (unsigned long)(send_lock_wait_total_us / frames_sent),
                         (unsigned long)(send_sock_total_us / frames_sent),
                         (unsigned long)frames_sent);
            }
        }
        return;
    }

    if (ws_send_image_frame(jpeg, size, stream_id) < 0) {
        ESP_LOGW(TAG, "image frame upload failed: stream=%u size=%u", (unsigned int)stream_id, (unsigned int)size);
    }

    ws_camera_finish_one_shot();
}

static esp_err_t ws_camera_ensure_ready(void) {
    esp_err_t ret;

    ESP_RETURN_ON_ERROR(ws_camera_ensure_lock(), TAG, "camera ws lock init failed");
    ESP_RETURN_ON_ERROR(camera_service_init(), TAG, "camera service init failed");

    if (xSemaphoreTake(s_camera_ctx.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    if (!s_camera_ctx.callback_registered) {
        ret = camera_service_register_frame_callback(ws_camera_frame_cb, NULL);
        if (ret == ESP_OK) {
            s_camera_ctx.callback_registered = true;
        }
    } else {
        ret = ESP_OK;
    }

    xSemaphoreGive(s_camera_ctx.lock);
    return ret;
}

static const char *ws_camera_command_type(const ws_capture_cmd_t *cmd) {
    if (!cmd) {
        return "ctrl.camera.unknown";
    }

    if (strcasecmp(cmd->action, "config") == 0) {
        return "ctrl.camera.video_config";
    }
    if (strcasecmp(cmd->action, "start") == 0) {
        return "ctrl.camera.start_video";
    }
    if (strcasecmp(cmd->action, "stop") == 0) {
        return "ctrl.camera.stop_video";
    }
    return "ctrl.camera.capture_image";
}

static void ws_camera_set_streaming(bool streaming, int fps) {
    if (ws_camera_ensure_lock() != ESP_OK) {
        return;
    }

    if (xSemaphoreTake(s_camera_ctx.lock, portMAX_DELAY) == pdTRUE) {
        s_camera_ctx.streaming = streaming;
        s_camera_ctx.target_fps = streaming ? fps : 0;
        xSemaphoreGive(s_camera_ctx.lock);
    }
}

static void ws_camera_get_cached_config(int *width, int *height, int *fps, int *quality) {
    if (width != NULL) {
        *width = 0;
    }
    if (height != NULL) {
        *height = 0;
    }
    if (fps != NULL) {
        *fps = 0;
    }
    if (quality != NULL) {
        *quality = WS_CAPTURE_DEFAULT_QUALITY;
    }

    if (ws_camera_ensure_lock() != ESP_OK) {
        return;
    }

    if (xSemaphoreTake(s_camera_ctx.lock, portMAX_DELAY) == pdTRUE) {
        if (width != NULL) {
            *width = s_camera_ctx.configured_width;
        }
        if (height != NULL) {
            *height = s_camera_ctx.configured_height;
        }
        if (fps != NULL) {
            *fps = s_camera_ctx.target_fps;
        }
        if (quality != NULL) {
            *quality = s_camera_ctx.configured_quality;
        }
        xSemaphoreGive(s_camera_ctx.lock);
    }
}

/* ------------------------------------------------------------------ */
/* Helper: Parse status data to determine emoji                       */
/* ------------------------------------------------------------------ */

const char *ws_status_data_to_emoji(const char *data) {
    if (!data || data[0] == '\0') {
        return NULL; /* No change */
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

void on_servo_handler(const ws_servo_cmd_t *cmd) {
    if (!cmd) {
        return;
    }

    /* Use ESP_LOGD to avoid flooding logs with high-frequency servo commands */
    ESP_LOGI(TAG, "Servo command: id=%s, angle=%d, time=%d", cmd->id, cmd->angle, cmd->time_ms);

    /* Send single servo command via HAL */
    hal_servo_send_cmd(cmd->id, cmd->angle, cmd->time_ms);
}

/* ------------------------------------------------------------------ */
/* Handler: Display Command                                           */
/* ------------------------------------------------------------------ */

void on_display_handler(const ws_display_cmd_t *cmd) {
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

void on_status_handler(const ws_status_cmd_t *cmd) {
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
/* Handler: Capture Command                                           */
/* ------------------------------------------------------------------ */

void on_capture_handler(const ws_capture_cmd_t *cmd) {
    esp_err_t ret;
    int fps = WS_CAPTURE_DEFAULT_FPS;
    int requested_width = 0;
    int requested_height = 0;
    int requested_quality = WS_CAPTURE_DEFAULT_QUALITY;
    int applied_width = 0;
    int applied_height = 0;
    int cached_fps = 0;
    uint16_t stream_id = 0;
    const char *command_type;
    char config_message[96];

    if (!cmd) {
        return;
    }

    command_type = ws_camera_command_type(cmd);
    ESP_LOGI(TAG,
             "Camera command: type=%s command_id=%s action=%s width=%d height=%d fps=%d quality=%d",
             command_type,
             cmd->command_id,
             cmd->action,
             cmd->width,
             cmd->height,
             cmd->fps,
             cmd->quality);
    ret = ws_camera_ensure_ready();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "camera path not ready: %s", esp_err_to_name(ret));
        ws_send_sys_nack(cmd->command_id, command_type, "camera_init_failed");
        return;
    }

    if (strcasecmp(cmd->action, "config") == 0) {
        ws_camera_get_cached_config(&requested_width, &requested_height, &cached_fps, &requested_quality);

        if (cmd->width > 0) {
            requested_width = cmd->width;
        }
        if (cmd->height > 0) {
            requested_height = cmd->height;
        }
        if (cmd->quality > 0) {
            requested_quality = cmd->quality;
        }

        if ((requested_width > 0) != (requested_height > 0)) {
            ws_send_sys_nack(cmd->command_id, command_type, "width_height_must_be_paired");
            return;
        }

        if (requested_width > 0 && requested_height > 0) {
            ret = camera_service_configure(requested_width, requested_height, requested_quality,
                                           &applied_width, &applied_height);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "camera config failed: %s", esp_err_to_name(ret));
                if (ret == ESP_ERR_NOT_SUPPORTED) {
                    ws_send_sys_nack(cmd->command_id, command_type, "unsupported_resolution");
                } else if (ret == ESP_ERR_INVALID_STATE) {
                    ws_send_sys_nack(cmd->command_id, command_type, "camera_busy");
                } else {
                    ws_send_sys_nack(cmd->command_id, command_type, "camera_config_failed");
                }
                return;
            }
        } else {
            applied_width = requested_width;
            applied_height = requested_height;
        }

        if (xSemaphoreTake(s_camera_ctx.lock, portMAX_DELAY) == pdTRUE) {
            if (applied_width > 0) {
                s_camera_ctx.configured_width = applied_width;
            }
            if (applied_height > 0) {
                s_camera_ctx.configured_height = applied_height;
            }
            if (cmd->fps > 0) {
                s_camera_ctx.target_fps = cmd->fps;
            }
            s_camera_ctx.configured_quality = requested_quality;
            xSemaphoreGive(s_camera_ctx.lock);
        }

        if (applied_width > 0 && applied_height > 0) {
            snprintf(config_message, sizeof(config_message), "applied=%dx%d quality_hint=%d",
                     applied_width, applied_height, requested_quality);
        } else {
            snprintf(config_message, sizeof(config_message), "fps=%d quality_hint=%d resolution_unchanged",
                     cmd->fps > 0 ? cmd->fps : cached_fps,
                     requested_quality);
        }

        ws_send_sys_ack(cmd->command_id, command_type, 0, "accepted");
        ws_send_camera_state("video_config", "accepted", 0, cmd->fps, config_message);
        return;
    }

    if (strcasecmp(cmd->action, "start") == 0) {
        if (camera_service_is_streaming()) {
            ws_send_sys_nack(cmd->command_id, command_type, "already_streaming");
            return;
        }

        if (xSemaphoreTake(s_camera_ctx.lock, portMAX_DELAY) == pdTRUE) {
            if (s_camera_ctx.target_fps > 0) {
                fps = s_camera_ctx.target_fps;
            }
            xSemaphoreGive(s_camera_ctx.lock);
        }
        if (cmd->fps > 0) {
            fps = cmd->fps;
        }
        if (fps > WS_CAPTURE_MAX_FPS) {
            fps = WS_CAPTURE_MAX_FPS;
        }
        if (fps <= 0) {
            fps = WS_CAPTURE_DEFAULT_FPS;
        }

        stream_id = ws_camera_begin_transfer(true, fps, cmd->command_id);
        if (stream_id == 0) {
            ws_send_sys_nack(cmd->command_id, command_type, "stream_id_alloc_failed");
            return;
        }

        ws_send_sys_ack(cmd->command_id, command_type, stream_id, "accepted");
        ret = camera_service_start_stream(fps);
        if (ret == ESP_OK) {
            ws_camera_set_streaming(true, fps);
            ws_send_camera_state("start_video", "started", stream_id, fps, "format=mjpeg");
            return;
        }

        ESP_LOGE(TAG, "camera stream start failed: %s", esp_err_to_name(ret));
        ws_camera_reset_stream(true);
        ws_send_camera_state("start_video", "error", stream_id, fps, "stream_start_failed");
        return;
    }

    if (strcasecmp(cmd->action, "stop") == 0) {
        uint32_t eos_seq = 1;
        uint16_t eos_stream_id = 0;

        if (xSemaphoreTake(s_camera_ctx.lock, portMAX_DELAY) == pdTRUE) {
            eos_stream_id = s_camera_ctx.current_stream_id;
            fps = s_camera_ctx.target_fps;
            xSemaphoreGive(s_camera_ctx.lock);
        }

        ws_send_sys_ack(cmd->command_id, command_type, eos_stream_id, "accepted");
        ret = camera_service_stop_stream();
        if (ret == ESP_OK) {
            if (xSemaphoreTake(s_camera_ctx.lock, portMAX_DELAY) == pdTRUE) {
                if (s_camera_ctx.current_stream_id != 0) {
                    eos_stream_id = s_camera_ctx.current_stream_id;
                }
                eos_seq = s_camera_ctx.next_seq;
                fps = s_camera_ctx.target_fps;
                xSemaphoreGive(s_camera_ctx.lock);
            }
            if (eos_stream_id != 0) {
                ws_send_video_end(eos_stream_id, eos_seq);
            }
            ws_send_camera_state("stop_video", "stopped", eos_stream_id, fps, NULL);
            ws_camera_reset_stream(true);
        } else {
            ESP_LOGE(TAG, "camera stream stop failed: %s", esp_err_to_name(ret));
            ws_send_camera_state("stop_video", "error", eos_stream_id, fps, "stream_stop_failed");
        }
        return;
    }

    stream_id = ws_camera_begin_transfer(false, 0, cmd->command_id);
    if (stream_id == 0) {
        ws_send_sys_nack(cmd->command_id, command_type, "stream_id_alloc_failed");
        return;
    }

    ws_send_sys_ack(cmd->command_id, command_type, stream_id, "accepted");
    ret = camera_service_capture_once();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "camera single capture failed: %s", esp_err_to_name(ret));
        ws_send_camera_state("capture_image", "error", stream_id, 0, "capture_failed");
        ws_camera_finish_one_shot();
        return;
    }

    ws_send_camera_state("capture_image", "completed", stream_id, 0, NULL);
}

/* ------------------------------------------------------------------ */
/* Handler: Reboot Command                                            */
/* ------------------------------------------------------------------ */

void on_reboot_handler(void) {
    ESP_LOGI(TAG, "Reboot command received");
    esp_restart();
}

/* ------------------------------------------------------------------ */
/* Handler: ASR Result (v2.0)                                         */
/* ------------------------------------------------------------------ */

void on_asr_result_handler(const ws_asr_result_cmd_t *cmd) {
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

void on_bot_reply_handler(const ws_bot_reply_cmd_t *cmd) {
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

void on_tts_end_handler(void) {
    ESP_LOGI(TAG, "TTS end received");

    /* Complete TTS playback and switch to happy */
    ws_tts_complete();
}

/* ------------------------------------------------------------------ */
/* Handler: Error Message (v2.0)                                      */
/* ------------------------------------------------------------------ */

void on_error_handler(const ws_error_cmd_t *cmd) {
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

ws_router_t ws_handlers_get_router(void) {
    ws_router_t router = {
        .on_servo = on_servo_handler,
        .on_display = on_display_handler,
        .on_status = on_status_handler,
        .on_capture = on_capture_handler,
        .on_reboot = on_reboot_handler,

        /* New handlers - v2.0 */
        .on_asr_result = on_asr_result_handler,
        .on_bot_reply = on_bot_reply_handler,
        .on_tts_end = on_tts_end_handler,
        .on_error = on_error_handler,
    };
    return router;
}
