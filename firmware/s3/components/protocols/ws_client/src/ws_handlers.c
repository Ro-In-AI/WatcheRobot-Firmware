/**
 * @file ws_handlers.c
 * @brief WebSocket message handlers implementation (Watcher protocol v0.1.5)
 */

#include "ws_handlers.h"

#include "camera_service.h"
#include "behavior_state_service.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal_servo.h"
#include "ws_client.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TAG "WS_HANDLERS"

#define SERVO_POSITION_REPORT_ENABLED 0
#define SERVO_REPORT_INTERVAL_MS 50
#define SERVO_REPORT_TASK_STACK 4096
#define SERVO_REPORT_TASK_PRIORITY 4
#define WS_CAPTURE_DEFAULT_FPS 5
#define WS_CAPTURE_MAX_FPS 10
#define WS_CAPTURE_DEFAULT_QUALITY 80
#define WS_CAMERA_UPLOAD_TASK_STACK 6144
#define WS_CAMERA_UPLOAD_TASK_PRIORITY 5
#define WS_DEVICE_ERROR_CODE_CAMERA 1502
#define WS_DEVICE_ERROR_CODE_SERVO 1503
#define WS_DEVICE_ERROR_CODE_OTA 1504

typedef struct {
    SemaphoreHandle_t lock;
    SemaphoreHandle_t frame_ready_sem;
    bool callback_registered;
    bool streaming;
    bool transfer_active;
    bool video_first_frame_pending;
    int target_fps;
    int configured_width;
    int configured_height;
    int configured_quality;
    uint32_t frames_sent;
    uint64_t send_total_us;
    uint64_t send_lock_wait_total_us;
    uint64_t send_sock_total_us;
    uint32_t frames_dropped;
    uint8_t *pending_jpeg;
    size_t pending_size;
    uint32_t pending_timestamp_ms;
    TaskHandle_t upload_task;
} ws_camera_context_t;

static ws_camera_context_t s_camera_ctx = {
    .lock = NULL,
    .frame_ready_sem = NULL,
    .callback_registered = false,
    .streaming = false,
    .transfer_active = false,
    .video_first_frame_pending = false,
    .target_fps = 0,
    .configured_width = 0,
    .configured_height = 0,
    .configured_quality = WS_CAPTURE_DEFAULT_QUALITY,
    .frames_sent = 0,
    .send_total_us = 0,
    .send_lock_wait_total_us = 0,
    .send_sock_total_us = 0,
    .frames_dropped = 0,
    .pending_jpeg = NULL,
    .pending_size = 0,
    .pending_timestamp_ms = 0,
    .upload_task = NULL,
};

#if SERVO_POSITION_REPORT_ENABLED
static TaskHandle_t s_servo_report_task = NULL;
#endif

static esp_err_t ws_camera_ensure_lock(void) {
    if (s_camera_ctx.lock == NULL) {
        s_camera_ctx.lock = xSemaphoreCreateMutex();
        if (s_camera_ctx.lock == NULL) {
            ESP_LOGE(TAG, "camera ws lock create failed");
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_camera_ctx.frame_ready_sem == NULL) {
        s_camera_ctx.frame_ready_sem = xSemaphoreCreateBinary();
        if (s_camera_ctx.frame_ready_sem == NULL) {
            ESP_LOGE(TAG, "camera ws frame semaphore create failed");
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

static void ws_camera_drain_frame_signal(void) {
    if (s_camera_ctx.frame_ready_sem == NULL) {
        return;
    }
    while (xSemaphoreTake(s_camera_ctx.frame_ready_sem, 0) == pdTRUE) {
    }
}

static void ws_camera_release_pending_locked(void) {
    if (s_camera_ctx.pending_jpeg != NULL) {
        free(s_camera_ctx.pending_jpeg);
        s_camera_ctx.pending_jpeg = NULL;
    }
    s_camera_ctx.pending_size = 0;
    s_camera_ctx.pending_timestamp_ms = 0;
}

static void ws_camera_reset_stats_locked(void) {
    s_camera_ctx.frames_sent = 0;
    s_camera_ctx.send_total_us = 0;
    s_camera_ctx.send_lock_wait_total_us = 0;
    s_camera_ctx.send_sock_total_us = 0;
    s_camera_ctx.frames_dropped = 0;
}

static esp_err_t ws_camera_begin_transfer(bool streaming, int fps) {
    ESP_RETURN_ON_ERROR(ws_camera_ensure_lock(), TAG, "camera ws lock init failed");

    if (xSemaphoreTake(s_camera_ctx.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    s_camera_ctx.streaming = streaming;
    s_camera_ctx.transfer_active = true;
    s_camera_ctx.video_first_frame_pending = streaming;
    s_camera_ctx.target_fps = streaming ? fps : 0;
    ws_camera_reset_stats_locked();
    ws_camera_release_pending_locked();
    ws_camera_drain_frame_signal();
    xSemaphoreGive(s_camera_ctx.lock);
    return ESP_OK;
}

static void ws_camera_finish_one_shot(void) {
    if (ws_camera_ensure_lock() != ESP_OK) {
        return;
    }

    if (xSemaphoreTake(s_camera_ctx.lock, portMAX_DELAY) == pdTRUE) {
        s_camera_ctx.streaming = false;
        s_camera_ctx.transfer_active = false;
        s_camera_ctx.video_first_frame_pending = false;
        s_camera_ctx.target_fps = 0;
        ws_camera_reset_stats_locked();
        ws_camera_release_pending_locked();
        ws_camera_drain_frame_signal();
        xSemaphoreGive(s_camera_ctx.lock);
    }
}

static void ws_camera_reset_stream(bool keep_config) {
    if (ws_camera_ensure_lock() != ESP_OK) {
        return;
    }

    if (xSemaphoreTake(s_camera_ctx.lock, portMAX_DELAY) == pdTRUE) {
        s_camera_ctx.streaming = false;
        s_camera_ctx.transfer_active = false;
        s_camera_ctx.video_first_frame_pending = false;
        s_camera_ctx.target_fps = 0;
        if (!keep_config) {
            s_camera_ctx.configured_width = 0;
            s_camera_ctx.configured_height = 0;
            s_camera_ctx.configured_quality = WS_CAPTURE_DEFAULT_QUALITY;
        }
        ws_camera_reset_stats_locked();
        ws_camera_release_pending_locked();
        ws_camera_drain_frame_signal();
        xSemaphoreGive(s_camera_ctx.lock);
    }
}

static void ws_camera_upload_task(void *arg) {
    (void)arg;

    while (true) {
        uint8_t *jpeg = NULL;
        size_t size = 0;
        uint32_t timestamp_ms = 0;
        bool first_frame = false;
        uint32_t frames_sent = 0;
        uint64_t send_total_us = 0;
        uint64_t send_lock_wait_total_us = 0;
        uint64_t send_sock_total_us = 0;
        uint32_t frames_dropped = 0;
        ws_client_media_send_stats_t send_stats = {0};
        int send_ret;

        if (xSemaphoreTake(s_camera_ctx.frame_ready_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (xSemaphoreTake(s_camera_ctx.lock, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!s_camera_ctx.streaming || !s_camera_ctx.transfer_active || s_camera_ctx.pending_jpeg == NULL ||
            s_camera_ctx.pending_size == 0U) {
            xSemaphoreGive(s_camera_ctx.lock);
            continue;
        }

        jpeg = s_camera_ctx.pending_jpeg;
        size = s_camera_ctx.pending_size;
        timestamp_ms = s_camera_ctx.pending_timestamp_ms;
        first_frame = s_camera_ctx.video_first_frame_pending;
        s_camera_ctx.video_first_frame_pending = false;
        s_camera_ctx.pending_jpeg = NULL;
        s_camera_ctx.pending_size = 0;
        s_camera_ctx.pending_timestamp_ms = 0;
        xSemaphoreGive(s_camera_ctx.lock);

        send_ret = ws_send_video_frame(jpeg, size, first_frame);
        ws_client_get_media_send_stats(&send_stats);
        if (send_ret < 0) {
            ESP_LOGW(TAG, "video frame upload failed: size=%u ts=%lu", (unsigned int)size, (unsigned long)timestamp_ms);
        } else if (send_stats.valid && xSemaphoreTake(s_camera_ctx.lock, portMAX_DELAY) == pdTRUE) {
            s_camera_ctx.frames_sent++;
            s_camera_ctx.send_total_us += send_stats.total_us;
            s_camera_ctx.send_lock_wait_total_us += send_stats.lock_wait_us;
            s_camera_ctx.send_sock_total_us += send_stats.send_us;
            frames_sent = s_camera_ctx.frames_sent;
            send_total_us = s_camera_ctx.send_total_us;
            send_lock_wait_total_us = s_camera_ctx.send_lock_wait_total_us;
            send_sock_total_us = s_camera_ctx.send_sock_total_us;
            frames_dropped = s_camera_ctx.frames_dropped;
            xSemaphoreGive(s_camera_ctx.lock);

            if ((frames_sent % 30U) == 0U) {
                ESP_LOGI(TAG,
                         "ws media timing latest_us{total=%lu lock=%lu send=%lu payload=%u packet=%u ts=%lu} "
                         "avg_us{total=%lu lock=%lu send=%lu} frames=%lu dropped=%lu",
                         (unsigned long)send_stats.total_us,
                         (unsigned long)send_stats.lock_wait_us,
                         (unsigned long)send_stats.send_us,
                         (unsigned int)send_stats.payload_len,
                         (unsigned int)send_stats.packet_len,
                         (unsigned long)timestamp_ms,
                         (unsigned long)(send_total_us / frames_sent),
                         (unsigned long)(send_lock_wait_total_us / frames_sent),
                         (unsigned long)(send_sock_total_us / frames_sent),
                         (unsigned long)frames_sent,
                         (unsigned long)frames_dropped);
            }
        }

        free(jpeg);
    }
}

static esp_err_t ws_camera_ensure_upload_task(void) {
    BaseType_t task_ret;

    ESP_RETURN_ON_ERROR(ws_camera_ensure_lock(), TAG, "camera ws lock init failed");

    if (xSemaphoreTake(s_camera_ctx.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    if (s_camera_ctx.upload_task != NULL) {
        xSemaphoreGive(s_camera_ctx.lock);
        return ESP_OK;
    }

    xSemaphoreGive(s_camera_ctx.lock);

    task_ret = xTaskCreate(ws_camera_upload_task,
                           "ws_cam_upload",
                           WS_CAMERA_UPLOAD_TASK_STACK,
                           NULL,
                           WS_CAMERA_UPLOAD_TASK_PRIORITY,
                           &s_camera_ctx.upload_task);
    if (task_ret != pdPASS) {
        if (xSemaphoreTake(s_camera_ctx.lock, portMAX_DELAY) == pdTRUE) {
            s_camera_ctx.upload_task = NULL;
            xSemaphoreGive(s_camera_ctx.lock);
        }
        ESP_LOGE(TAG, "camera upload task create failed");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void ws_camera_frame_cb(const uint8_t *jpeg, size_t size, uint32_t timestamp_ms, void *ctx) {
    bool streaming = false;
    bool transfer_active = false;
    uint8_t *jpeg_copy = NULL;

    (void)ctx;

    if (jpeg == NULL || size == 0U) {
        return;
    }

    if (ws_camera_ensure_lock() != ESP_OK) {
        return;
    }

    if (xSemaphoreTake(s_camera_ctx.lock, portMAX_DELAY) == pdTRUE) {
        streaming = s_camera_ctx.streaming;
        transfer_active = s_camera_ctx.transfer_active;
        xSemaphoreGive(s_camera_ctx.lock);
    }

    if (!transfer_active) {
        ESP_LOGW(TAG, "camera frame dropped: transfer not active");
        return;
    }

    if (streaming) {
        jpeg_copy = (uint8_t *)malloc(size);
        if (jpeg_copy == NULL) {
            ESP_LOGW(TAG, "camera frame drop: alloc failed size=%u", (unsigned int)size);
            if (xSemaphoreTake(s_camera_ctx.lock, portMAX_DELAY) == pdTRUE) {
                s_camera_ctx.frames_dropped++;
                xSemaphoreGive(s_camera_ctx.lock);
            }
            return;
        }

        memcpy(jpeg_copy, jpeg, size);
        if (xSemaphoreTake(s_camera_ctx.lock, portMAX_DELAY) == pdTRUE) {
            if (s_camera_ctx.pending_jpeg != NULL) {
                free(s_camera_ctx.pending_jpeg);
                s_camera_ctx.frames_dropped++;
            }
            s_camera_ctx.pending_jpeg = jpeg_copy;
            s_camera_ctx.pending_size = size;
            s_camera_ctx.pending_timestamp_ms = timestamp_ms;
            xSemaphoreGive(s_camera_ctx.lock);
            xSemaphoreGive(s_camera_ctx.frame_ready_sem);
        } else {
            free(jpeg_copy);
        }
        return;
    }

    if (ws_send_image_frame(jpeg, size) < 0) {
        ESP_LOGW(TAG, "image frame upload failed: size=%u", (unsigned int)size);
        ws_send_device_error(WS_DEVICE_ERROR_CODE_CAMERA, "image_upload_failed");
        ws_send_camera_state("capture_image", "error", 0, "image_upload_failed");
    } else {
        ws_send_camera_state("capture_image", "completed", 0, NULL);
    }

    ws_camera_finish_one_shot();
}

static esp_err_t ws_camera_ensure_ready(void) {
    esp_err_t ret;

    ESP_RETURN_ON_ERROR(ws_camera_ensure_lock(), TAG, "camera ws lock init failed");
    ESP_RETURN_ON_ERROR(ws_camera_ensure_upload_task(), TAG, "camera upload task init failed");
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
    if (cmd == NULL) {
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

static bool ws_contains_nocase(const char *haystack, const char *needle) {
    size_t needle_len;

    if (haystack == NULL || needle == NULL || needle[0] == '\0') {
        return false;
    }

    needle_len = strlen(needle);
    while (*haystack != '\0') {
        if (strncasecmp(haystack, needle, needle_len) == 0) {
            return true;
        }
        haystack++;
    }

    return false;
}

static void ws_normalize_resource_name(const char *raw, char *out, size_t out_size) {
    const char *base;
    const char *ext;
    size_t len;
    size_t i;

    if (out == NULL || out_size == 0) {
        return;
    }

    out[0] = '\0';
    if (raw == NULL || raw[0] == '\0') {
        return;
    }

    base = raw;
    for (i = 0; raw[i] != '\0'; ++i) {
        if (raw[i] == '/' || raw[i] == '\\') {
            base = &raw[i + 1];
        }
    }

    ext = strrchr(base, '.');
    len = (ext != NULL && ext > base) ? (size_t)(ext - base) : strlen(base);
    if (len >= out_size) {
        len = out_size - 1;
    }

    for (i = 0; i < len; ++i) {
        out[i] = (char)tolower((unsigned char)base[i]);
    }
    out[len] = '\0';
}

static bool ws_append_state_candidate(const char **candidates, size_t *count, size_t max_count, const char *candidate) {
    size_t i;

    if (candidates == NULL || count == NULL || candidate == NULL || candidate[0] == '\0') {
        return false;
    }

    for (i = 0; i < *count; ++i) {
        if (strcasecmp(candidates[i], candidate) == 0) {
            return false;
        }
    }

    if (*count >= max_count) {
        return false;
    }

    candidates[*count] = candidate;
    (*count)++;
    return true;
}

#if SERVO_POSITION_REPORT_ENABLED
static void ws_servo_report_task(void *arg) {
    bool session_ready_seen = false;
    bool motion_active = false;
    bool final_report_sent = false;
    int last_observed_x = -1;
    int last_observed_y = -1;
    int last_reported_x = -1;
    int last_reported_y = -1;

    (void)arg;

    while (true) {
        int x_deg;
        int y_deg;
        bool suppress_action_reports;

        if (!ws_client_is_session_ready()) {
            session_ready_seen = false;
            motion_active = false;
            final_report_sent = false;
            last_observed_x = -1;
            last_observed_y = -1;
            last_reported_x = -1;
            last_reported_y = -1;
            vTaskDelay(pdMS_TO_TICKS(SERVO_REPORT_INTERVAL_MS));
            continue;
        }

        x_deg = hal_servo_get_angle(SERVO_AXIS_X);
        y_deg = hal_servo_get_angle(SERVO_AXIS_Y);
        if (x_deg < 0 || y_deg < 0) {
            vTaskDelay(pdMS_TO_TICKS(SERVO_REPORT_INTERVAL_MS));
            continue;
        }

        if (!session_ready_seen) {
            ws_send_servo_position((float)x_deg, (float)y_deg);
            session_ready_seen = true;
            motion_active = false;
            final_report_sent = true;
            last_observed_x = x_deg;
            last_observed_y = y_deg;
            last_reported_x = x_deg;
            last_reported_y = y_deg;
            vTaskDelay(pdMS_TO_TICKS(SERVO_REPORT_INTERVAL_MS));
            continue;
        }

        suppress_action_reports = behavior_state_is_action_active();
        if (suppress_action_reports) {
            motion_active = false;
            final_report_sent = true;
            last_observed_x = x_deg;
            last_observed_y = y_deg;
            last_reported_x = x_deg;
            last_reported_y = y_deg;
            vTaskDelay(pdMS_TO_TICKS(SERVO_REPORT_INTERVAL_MS));
            continue;
        }

        if (x_deg != last_observed_x || y_deg != last_observed_y) {
            motion_active = true;
            final_report_sent = false;
            last_observed_x = x_deg;
            last_observed_y = y_deg;
            if (x_deg != last_reported_x || y_deg != last_reported_y) {
                ws_send_servo_position((float)x_deg, (float)y_deg);
                last_reported_x = x_deg;
                last_reported_y = y_deg;
            }
        } else if (motion_active && !final_report_sent) {
            ws_send_servo_position((float)x_deg, (float)y_deg);
            motion_active = false;
            final_report_sent = true;
            last_reported_x = x_deg;
            last_reported_y = y_deg;
        }

        vTaskDelay(pdMS_TO_TICKS(SERVO_REPORT_INTERVAL_MS));
    }
}
#endif

void ws_handlers_init(void) {
#if SERVO_POSITION_REPORT_ENABLED
    if (s_servo_report_task == NULL) {
        if (xTaskCreate(ws_servo_report_task,
                        "ws_servo_report",
                        SERVO_REPORT_TASK_STACK,
                        NULL,
                        SERVO_REPORT_TASK_PRIORITY,
                        &s_servo_report_task) != pdPASS) {
            s_servo_report_task = NULL;
            ESP_LOGE(TAG, "servo report task create failed");
        }
    }
#else
    ESP_LOGI(TAG, "Servo position reporting disabled");
#endif
}

const char *ws_ai_status_to_emoji(const char *status, const char *message) {
    if (ws_contains_nocase(status, "thinking") || ws_contains_nocase(message, "thinking")) {
        return "thinking";
    }
    if (ws_contains_nocase(status, "processing") || ws_contains_nocase(status, "analyzing") ||
        ws_contains_nocase(message, "processing") || ws_contains_nocase(message, "analyzing")) {
        return "processing";
    }
    if (ws_contains_nocase(status, "speaking") || ws_contains_nocase(message, "speaking")) {
        return "speaking";
    }
    if (ws_contains_nocase(status, "idle") || ws_contains_nocase(message, "idle")) {
        return "standby";
    }
    if (ws_contains_nocase(status, "done") || ws_contains_nocase(status, "completed") ||
        ws_contains_nocase(message, "done") || ws_contains_nocase(message, "completed")) {
        return "happy";
    }
    if (ws_contains_nocase(status, "error") || ws_contains_nocase(status, "fail") ||
        ws_contains_nocase(message, "error") || ws_contains_nocase(message, "fail")) {
        return "error";
    }
    return NULL;
}

void on_sys_ack_handler(const ws_sys_ack_t *msg) {
    if (msg == NULL) {
        return;
    }

    ESP_LOGI(TAG, "sys.ack: type=%s command_id=%s code=%d message=%s", msg->type, msg->command_id, msg->code,
             msg->message);
    if (strcmp(msg->type, "sys.client.hello") == 0) {
        ws_client_mark_hello_acked();
    }
}

void on_sys_nack_handler(const ws_sys_nack_t *msg) {
    if (msg == NULL) {
        return;
    }

    ESP_LOGW(TAG, "sys.nack: type=%s command_id=%s code=%d reason=%s", msg->type, msg->command_id, msg->code,
             msg->reason);
    if (strcmp(msg->type, "sys.client.hello") == 0) {
        behavior_state_set_with_text("error", msg->reason[0] != '\0' ? msg->reason : "Hello Rejected", 0);
    } else if (msg->reason[0] != '\0') {
        behavior_state_set_with_text("error", msg->reason, 0);
    }
}

void on_sys_ping_handler(void) {
    ws_send_sys_pong();
}

void on_sys_pong_handler(void) {
    ESP_LOGD(TAG, "sys.pong received");
}

void on_session_resume_handler(void) {
    ESP_LOGW(TAG, "sys.session.resume ignored");
}

void on_servo_handler(const ws_servo_cmd_t *cmd) {
    esp_err_t ret;
    int x_deg;
    int y_deg;
    int duration_ms;

    if (cmd == NULL) {
        return;
    }

    if (!cmd->has_x || !cmd->has_y) {
        ws_send_sys_nack("ctrl.servo.angle", NULL, "invalid_servo_payload");
        return;
    }

    x_deg = (int)(cmd->x_deg + 0.5f);
    y_deg = (int)(cmd->y_deg + 0.5f);
    duration_ms = cmd->duration_ms;
    if (duration_ms <= 0) {
        ws_send_sys_nack("ctrl.servo.angle", NULL, "invalid_duration_ms");
        return;
    }
    if (x_deg < 0 || x_deg > 180 || y_deg < 0 || y_deg > 180) {
        ws_send_sys_nack("ctrl.servo.angle", NULL, "angle_out_of_range");
        return;
    }

    ESP_LOGI(TAG, "servo command: x=%d y=%d duration_ms=%d", x_deg, y_deg, duration_ms);
    ret = hal_servo_move_sync(x_deg, y_deg, duration_ms);
    if (ret == ESP_OK) {
        ws_send_sys_ack("ctrl.servo.angle", NULL);
    } else {
        ws_send_device_error(WS_DEVICE_ERROR_CODE_SERVO, "servo_move_failed");
        ws_send_sys_nack("ctrl.servo.angle", NULL, "servo_move_failed");
    }
}

void on_state_set_handler(const ws_state_cmd_t *cmd) {
    esp_err_t ret;

    if (cmd == NULL || cmd->state_id[0] == '\0') {
        ws_send_sys_nack("ctrl.robot.state.set", cmd != NULL ? cmd->command_id : NULL, "invalid_state_payload");
        return;
    }

    ret = behavior_state_set(cmd->state_id);
    if (ret == ESP_OK) {
        ws_send_sys_ack("ctrl.robot.state.set", cmd->command_id);
    } else if (ret == ESP_ERR_NOT_FOUND) {
        ws_send_sys_nack("ctrl.robot.state.set", cmd->command_id, "unknown_state_id");
    } else {
        ws_send_sys_nack("ctrl.robot.state.set", cmd->command_id, "state_set_failed");
    }
}

void on_capture_handler(const ws_capture_cmd_t *cmd) {
    esp_err_t ret;
    int fps = WS_CAPTURE_DEFAULT_FPS;
    int requested_width = 0;
    int requested_height = 0;
    int requested_quality = WS_CAPTURE_DEFAULT_QUALITY;
    int applied_width = 0;
    int applied_height = 0;
    int cached_fps = 0;
    const char *command_type;
    char config_message[96];

    if (cmd == NULL) {
        return;
    }

    command_type = ws_camera_command_type(cmd);
    ESP_LOGI(TAG,
             "camera command: type=%s command_id=%s action=%s width=%d height=%d fps=%d quality=%d",
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
        ws_send_device_error(WS_DEVICE_ERROR_CODE_CAMERA, "camera_init_failed");
        ws_send_sys_nack(command_type, cmd->command_id, "camera_init_failed");
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
            ws_send_sys_nack(command_type, cmd->command_id, "width_height_must_be_paired");
            return;
        }

        if (requested_width > 0 && requested_height > 0) {
            ret = camera_service_configure(requested_width, requested_height, requested_quality, &applied_width,
                                           &applied_height);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "camera config failed: %s", esp_err_to_name(ret));
                if (ret == ESP_ERR_NOT_SUPPORTED) {
                    ws_send_sys_nack(command_type, cmd->command_id, "unsupported_resolution");
                } else if (ret == ESP_ERR_INVALID_STATE) {
                    ws_send_device_error(WS_DEVICE_ERROR_CODE_CAMERA, "camera_busy");
                    ws_send_sys_nack(command_type, cmd->command_id, "camera_busy");
                } else {
                    ws_send_device_error(WS_DEVICE_ERROR_CODE_CAMERA, "camera_config_failed");
                    ws_send_sys_nack(command_type, cmd->command_id, "camera_config_failed");
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
            snprintf(config_message, sizeof(config_message), "applied=%dx%d quality_hint=%d", applied_width,
                     applied_height, requested_quality);
        } else {
            snprintf(config_message,
                     sizeof(config_message),
                     "fps=%d quality_hint=%d resolution_unchanged",
                     cmd->fps > 0 ? cmd->fps : cached_fps,
                     requested_quality);
        }

        ws_send_sys_ack(command_type, cmd->command_id);
        ws_send_camera_state("video_config", "accepted", cmd->fps > 0 ? cmd->fps : cached_fps, config_message);
        return;
    }

    if (strcasecmp(cmd->action, "start") == 0) {
        if (camera_service_is_streaming()) {
            ws_send_sys_nack(command_type, cmd->command_id, "already_streaming");
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

        if (ws_camera_begin_transfer(true, fps) != ESP_OK) {
            ws_send_sys_nack(command_type, cmd->command_id, "camera_state_unavailable");
            return;
        }

        ws_send_sys_ack(command_type, cmd->command_id);
        ret = camera_service_start_stream(fps);
        if (ret == ESP_OK) {
            ws_send_camera_state("start_video", "started", fps, "format=mjpeg");
            return;
        }

        ESP_LOGE(TAG, "camera stream start failed: %s", esp_err_to_name(ret));
        ws_camera_reset_stream(true);
        ws_send_device_error(WS_DEVICE_ERROR_CODE_CAMERA, "stream_start_failed");
        ws_send_camera_state("start_video", "error", fps, "stream_start_failed");
        return;
    }

    if (strcasecmp(cmd->action, "stop") == 0) {
        if (!camera_service_is_streaming()) {
            ws_send_sys_nack(command_type, cmd->command_id, "not_streaming");
            return;
        }

        if (xSemaphoreTake(s_camera_ctx.lock, portMAX_DELAY) == pdTRUE) {
            fps = s_camera_ctx.target_fps;
            xSemaphoreGive(s_camera_ctx.lock);
        }

        ws_send_sys_ack(command_type, cmd->command_id);
        ret = camera_service_stop_stream();
        if (ret == ESP_OK) {
            ws_send_video_end();
            ws_send_camera_state("stop_video", "stopped", fps, NULL);
            ws_camera_reset_stream(true);
        } else {
            ESP_LOGE(TAG, "camera stream stop failed: %s", esp_err_to_name(ret));
            ws_send_device_error(WS_DEVICE_ERROR_CODE_CAMERA, "stream_stop_failed");
            ws_send_camera_state("stop_video", "error", fps, "stream_stop_failed");
        }
        return;
    }

    if (ws_camera_begin_transfer(false, 0) != ESP_OK) {
        ws_send_sys_nack(command_type, cmd->command_id, "camera_state_unavailable");
        return;
    }

    ws_send_sys_ack(command_type, cmd->command_id);
    ret = camera_service_capture_once();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "camera single capture failed: %s", esp_err_to_name(ret));
        ws_send_device_error(WS_DEVICE_ERROR_CODE_CAMERA, "capture_failed");
        ws_send_camera_state("capture_image", "error", 0, "capture_failed");
        ws_camera_finish_one_shot();
    }
}

void on_asr_result_handler(const ws_text_event_t *event) {
    if (event == NULL) {
        return;
    }

    ESP_LOGI(TAG, "ASR result: %s", event->text);
    behavior_state_set_with_text("processing", event->text[0] != '\0' ? event->text : "Listening...", 0);
}

void on_ai_status_handler(const ws_ai_status_t *event) {
    char action_state_id[WS_STATE_ID_MAX];
    char status_state_id[WS_STATE_ID_MAX];
    char fallback_state_id[WS_STATE_ID_MAX];
    char image_name[WS_STATE_ID_MAX];
    char sound_id[WS_STATE_ID_MAX];
    const char *state_candidates[3] = {0};
    const char *action_candidates[3] = {0};
    size_t state_candidate_count = 0;
    size_t action_candidate_count = 0;
    const char *selected_action_id = NULL;
    const char *text;
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    size_t i;

    if (event == NULL) {
        return;
    }

    ws_normalize_resource_name(event->action_file, action_state_id, sizeof(action_state_id));
    ws_normalize_resource_name(event->status, status_state_id, sizeof(status_state_id));
    ws_normalize_resource_name(event->image_name, image_name, sizeof(image_name));
    ws_normalize_resource_name(event->sound_file, sound_id, sizeof(sound_id));
    ws_normalize_resource_name(ws_ai_status_to_emoji(event->status, event->message),
                               fallback_state_id,
                               sizeof(fallback_state_id));

    text = event->message[0] != '\0' ? event->message : NULL;

    ws_append_state_candidate(state_candidates, &state_candidate_count, 3, action_state_id);
    ws_append_state_candidate(state_candidates, &state_candidate_count, 3, status_state_id);
    ws_append_state_candidate(state_candidates, &state_candidate_count, 3, fallback_state_id);
    ws_append_state_candidate(action_candidates, &action_candidate_count, 3, action_state_id);
    ws_append_state_candidate(action_candidates, &action_candidate_count, 3, status_state_id);
    ws_append_state_candidate(action_candidates, &action_candidate_count, 3, fallback_state_id);

    for (i = 0; i < action_candidate_count; ++i) {
        if (behavior_state_has_action(action_candidates[i])) {
            selected_action_id = action_candidates[i];
            break;
        }
    }

    ESP_LOGI(TAG,
             "AI status: status=%s message=%s image=%s action=%s sound=%s selected_action=%s",
             event->status,
             event->message,
             event->image_name,
             event->action_file,
             event->sound_file,
             selected_action_id != NULL ? selected_action_id : "<none>");

    for (i = 0; i < state_candidate_count; ++i) {
        ret = behavior_state_set_with_resources_and_action(state_candidates[i],
                                                           text,
                                                           0,
                                                           image_name[0] != '\0' ? image_name : NULL,
                                                           sound_id[0] != '\0' ? sound_id : NULL,
                                                           selected_action_id);
        if (ret != ESP_ERR_NOT_FOUND) {
            break;
        }
    }

    if (ret == ESP_ERR_NOT_FOUND) {
        ret = behavior_state_set_with_resources_and_action("standby",
                                                           text,
                                                           0,
                                                           image_name[0] != '\0' ? image_name : NULL,
                                                           sound_id[0] != '\0' ? sound_id : NULL,
                                                           selected_action_id);
    }

    if (ret != ESP_OK && text != NULL) {
        behavior_state_set_text(text, 0);
    }
}

void on_ai_thinking_handler(const ws_ai_thinking_t *event) {
    if (event == NULL) {
        return;
    }

    ESP_LOGI(TAG, "AI thinking: kind=%s content=%s", event->kind, event->content);
    if (event->content[0] != '\0') {
        behavior_state_set_with_text("thinking", event->content, 0);
    }
}

void on_ai_reply_handler(const ws_text_event_t *event) {
    if (event == NULL) {
        return;
    }

    ESP_LOGI(TAG, "AI reply: %s", event->text);
}

void on_transfer_handler(const ws_transfer_cmd_t *cmd) {
    if (cmd == NULL) {
        return;
    }

    ESP_LOGW(TAG, "transfer not supported: type=%s transfer_id=%s", cmd->message_type, cmd->transfer_id);
    if (strcmp(cmd->message_type, "xfer.ota.handshake") == 0) {
        ws_send_ota_handshake(cmd->transfer_id[0] != '\0' ? cmd->transfer_id : NULL, "not_supported");
    }
    ws_send_ota_progress(0, "rejected", "not_supported");
    ws_send_device_error(WS_DEVICE_ERROR_CODE_OTA, "ota_not_supported");
    ws_send_sys_nack(cmd->message_type,
                     cmd->transfer_id[0] != '\0' ? cmd->transfer_id : NULL,
                     "not_supported");
}

ws_router_t ws_handlers_get_router(void) {
    ws_router_t router = {
        .on_sys_ack = on_sys_ack_handler,
        .on_sys_nack = on_sys_nack_handler,
        .on_sys_ping = on_sys_ping_handler,
        .on_sys_pong = on_sys_pong_handler,
        .on_session_resume = on_session_resume_handler,
        .on_servo = on_servo_handler,
        .on_state_set = on_state_set_handler,
        .on_capture = on_capture_handler,
        .on_asr_result = on_asr_result_handler,
        .on_ai_status = on_ai_status_handler,
        .on_ai_thinking = on_ai_thinking_handler,
        .on_ai_reply = on_ai_reply_handler,
        .on_transfer = on_transfer_handler,
    };
    return router;
}
