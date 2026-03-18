#include "hal_camera.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "sensecap-watcher.h"
#include "sscma_client.h"

#define TAG "HAL_CAMERA"
#define HAL_CAMERA_CONNECT_TIMEOUT_MS CONFIG_WATCHER_CAMERA_CONNECT_TIMEOUT_MS
#define HAL_CAMERA_CAPTURE_TIMEOUT_MS CONFIG_WATCHER_CAMERA_CAPTURE_TIMEOUT_MS
#define HAL_CAMERA_MAX_FPS 30
#define HAL_CAMERA_STREAM_TASK_STACK 6144
#define HAL_CAMERA_STREAM_TASK_PRIORITY 5
#define HAL_CAMERA_STREAM_STOP_TIMEOUT_MS (HAL_CAMERA_CAPTURE_TIMEOUT_MS + 1000)
#define HAL_CAMERA_STREAM_LOG_EVERY 30

typedef struct {
    sscma_client_handle_t client;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t connect_sem;
    SemaphoreHandle_t capture_sem;
    TaskHandle_t stream_task;
    hal_camera_frame_cb_t stream_cb;
    void *stream_ctx;
    int stream_fps;
    bool initialized;
    bool connected;
    bool init_in_progress;
    bool callbacks_registered;
    bool client_started;
    bool capture_in_progress;
    bool streaming;
    bool stream_stop_requested;
    esp_err_t init_status;
    esp_err_t capture_status;
    char *capture_image;
    int capture_image_size;
    uint32_t stream_frames_ok;
    uint32_t stream_frames_err;
} hal_camera_context_t;

static hal_camera_context_t s_ctx = {
    .client = NULL,
    .lock = NULL,
    .connect_sem = NULL,
    .capture_sem = NULL,
    .stream_task = NULL,
    .stream_cb = NULL,
    .stream_ctx = NULL,
    .stream_fps = 0,
    .initialized = false,
    .connected = false,
    .init_in_progress = false,
    .callbacks_registered = false,
    .client_started = false,
    .capture_in_progress = false,
    .streaming = false,
    .stream_stop_requested = false,
    .init_status = ESP_OK,
    .capture_status = ESP_OK,
    .capture_image = NULL,
    .capture_image_size = 0,
    .stream_frames_ok = 0,
    .stream_frames_err = 0,
};

static void hal_camera_clear_capture_locked(void) {
    if (s_ctx.capture_image != NULL) {
        free(s_ctx.capture_image);
        s_ctx.capture_image = NULL;
    }
    s_ctx.capture_image_size = 0;
    s_ctx.capture_status = ESP_OK;
}

static void hal_camera_signal_connect(void) {
    s_ctx.connected = true;
    if (s_ctx.connect_sem != NULL) {
        xSemaphoreGive(s_ctx.connect_sem);
    }
}

static bool hal_camera_extract_capture(const sscma_client_reply_t *reply) {
    char *image = NULL;
    int image_size = 0;
    bool accepted = false;

    if (reply == NULL || reply->payload == NULL) {
        return false;
    }

    if (sscma_utils_fetch_image_from_reply(reply, &image, &image_size) != ESP_OK || image == NULL || image_size <= 0) {
        return false;
    }

    if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) == pdTRUE) {
        if (s_ctx.capture_in_progress && s_ctx.capture_image == NULL) {
            s_ctx.capture_image = image;
            s_ctx.capture_image_size = image_size;
            s_ctx.capture_status = ESP_OK;
            s_ctx.capture_in_progress = false;
            accepted = true;
        }
        xSemaphoreGive(s_ctx.lock);
    }

    if (!accepted) {
        free(image);
    }

    if (accepted && s_ctx.capture_sem != NULL) {
        xSemaphoreGive(s_ctx.capture_sem);
    }

    return accepted;
}

static void hal_camera_on_connect(sscma_client_handle_t client, const sscma_client_reply_t *reply, void *user_ctx) {
    (void)client;
    (void)reply;
    (void)user_ctx;

    if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) == pdTRUE) {
        ESP_LOGI(TAG, "HX6538 connect event received");
        hal_camera_signal_connect();
        xSemaphoreGive(s_ctx.lock);
    }
}

static void hal_camera_on_response(sscma_client_handle_t client, const sscma_client_reply_t *reply, void *user_ctx) {
    (void)client;
    (void)user_ctx;
    (void)hal_camera_extract_capture(reply);
}

static void hal_camera_on_event(sscma_client_handle_t client, const sscma_client_reply_t *reply, void *user_ctx) {
    (void)client;
    (void)user_ctx;
    (void)hal_camera_extract_capture(reply);
}

static esp_err_t hal_camera_ensure_sync_primitives(void) {
    if (s_ctx.lock == NULL) {
        s_ctx.lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_ctx.lock != NULL, ESP_ERR_NO_MEM, TAG, "create lock failed");
    }

    if (s_ctx.connect_sem == NULL) {
        s_ctx.connect_sem = xSemaphoreCreateBinary();
        ESP_RETURN_ON_FALSE(s_ctx.connect_sem != NULL, ESP_ERR_NO_MEM, TAG, "create connect semaphore failed");
    }

    if (s_ctx.capture_sem == NULL) {
        s_ctx.capture_sem = xSemaphoreCreateBinary();
        ESP_RETURN_ON_FALSE(s_ctx.capture_sem != NULL, ESP_ERR_NO_MEM, TAG, "create capture semaphore failed");
    }

    return ESP_OK;
}

static void hal_camera_drain_semaphore(SemaphoreHandle_t sem) {
    if (sem == NULL) {
        return;
    }

    while (xSemaphoreTake(sem, 0) == pdTRUE) {
    }
}

static esp_err_t hal_camera_log_device_info(void) {
    sscma_client_info_t *info = NULL;
    sscma_client_model_t *model = NULL;
    sscma_client_sensor_t sensor = {0};
    esp_err_t ret;

    ret = sscma_client_get_info(s_ctx.client, &info, false);
    if (ret == ESP_OK && info != NULL) {
        ESP_LOGI(TAG, "HX6538 id=%s name=%s hw=%s fw=%s at=%s",
                 info->id ? info->id : "<null>",
                 info->name ? info->name : "<null>",
                 info->hw_ver ? info->hw_ver : "<null>",
                 info->fw_ver ? info->fw_ver : "<null>",
                 info->sw_ver ? info->sw_ver : "<null>");
    } else {
        ESP_LOGW(TAG, "sscma_client_get_info failed: %s", esp_err_to_name(ret));
    }

    ret = sscma_client_get_model(s_ctx.client, &model, false);
    if (ret == ESP_OK && model != NULL) {
        ESP_LOGI(TAG, "HX6538 model id=%d uuid=%s name=%s ver=%s",
                 model->id,
                 model->uuid ? model->uuid : "<null>",
                 model->name ? model->name : "<null>",
                 model->ver ? model->ver : "<null>");
    } else {
        ESP_LOGW(TAG, "sscma_client_get_model failed: %s", esp_err_to_name(ret));
    }

    ret = sscma_client_get_sensor(s_ctx.client, &sensor);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "HX6538 sensor id=%d type=%d state=%d opt_id=%d detail=%s",
                 sensor.id, sensor.type, sensor.state, sensor.opt_id,
                 sensor.opt_detail ? sensor.opt_detail : "<null>");
        free(sensor.opt_detail);
    } else {
        ESP_LOGW(TAG, "sscma_client_get_sensor failed: %s", esp_err_to_name(ret));
    }

    return ESP_OK;
}

static const char *hal_camera_trim_image_string(const char *image, size_t *len) {
    const char *payload = image;
    size_t payload_len;

    if (image == NULL || len == NULL) {
        return NULL;
    }

    if (strncmp(payload, "data:", 5) == 0) {
        const char *comma = strchr(payload, ',');
        if (comma != NULL) {
            payload = comma + 1;
        }
    }

    while (*payload != '\0' && isspace((unsigned char)*payload)) {
        payload++;
    }

    payload_len = strlen(payload);
    while (payload_len > 0 && isspace((unsigned char)payload[payload_len - 1])) {
        payload_len--;
    }

    *len = payload_len;
    return payload;
}

static esp_err_t hal_camera_decode_image(const char *image, uint8_t **jpeg, size_t *jpeg_size) {
    const char *payload;
    size_t payload_len = 0;
    size_t max_output = 0;
    size_t decoded_len = 0;
    uint8_t *buffer = NULL;
    int ret;

    ESP_RETURN_ON_FALSE(image != NULL && jpeg != NULL && jpeg_size != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "invalid decode arguments");

    payload = hal_camera_trim_image_string(image, &payload_len);
    ESP_RETURN_ON_FALSE(payload != NULL && payload_len > 0, ESP_ERR_INVALID_ARG, TAG, "image payload empty");

    max_output = ((payload_len + 3) / 4) * 3 + 4;
    buffer = (uint8_t *)malloc(max_output);
    ESP_RETURN_ON_FALSE(buffer != NULL, ESP_ERR_NO_MEM, TAG, "alloc jpeg buffer failed");

    ret = mbedtls_base64_decode(buffer, max_output, &decoded_len, (const unsigned char *)payload, payload_len);
    if (ret != 0) {
        free(buffer);
        ESP_LOGW(TAG, "base64 decode failed: -0x%04x", (unsigned int)(-ret));
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (decoded_len < 2 || buffer[0] != 0xFF || buffer[1] != 0xD8) {
        free(buffer);
        ESP_LOGW(TAG, "decoded payload is not a JPEG, len=%u", (unsigned int)decoded_len);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *jpeg = buffer;
    *jpeg_size = decoded_len;
    return ESP_OK;
}

static esp_err_t hal_camera_prepare_capture(bool from_stream) {
    ESP_RETURN_ON_ERROR(hal_camera_init(), TAG, "hal_camera_init failed");

    if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    if (s_ctx.capture_in_progress) {
        xSemaphoreGive(s_ctx.lock);
        ESP_LOGW(TAG, "capture already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ctx.streaming && !from_stream) {
        xSemaphoreGive(s_ctx.lock);
        ESP_LOGW(TAG, "capture rejected while streaming");
        return ESP_ERR_INVALID_STATE;
    }

    hal_camera_clear_capture_locked();
    s_ctx.capture_in_progress = true;
    s_ctx.capture_status = ESP_ERR_TIMEOUT;
    xSemaphoreGive(s_ctx.lock);

    hal_camera_drain_semaphore(s_ctx.capture_sem);
    return ESP_OK;
}

static void hal_camera_abort_capture(void) {
    if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) == pdTRUE) {
        s_ctx.capture_in_progress = false;
        hal_camera_clear_capture_locked();
        xSemaphoreGive(s_ctx.lock);
    }
}

static esp_err_t hal_camera_take_image_string(bool from_stream, char **image, int *image_size) {
    esp_err_t ret;

    ESP_RETURN_ON_FALSE(image != NULL && image_size != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid image output");
    *image = NULL;
    *image_size = 0;

    ESP_RETURN_ON_ERROR(hal_camera_prepare_capture(from_stream), TAG, "prepare capture failed");

    ret = sscma_client_invoke(s_ctx.client, 1, false, true);
    if (ret != ESP_OK) {
        hal_camera_abort_capture();
        ESP_LOGE(TAG, "sscma invoke failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (xSemaphoreTake(s_ctx.capture_sem, pdMS_TO_TICKS(HAL_CAMERA_CAPTURE_TIMEOUT_MS)) != pdTRUE) {
        hal_camera_abort_capture();
        ESP_LOGW(TAG, "capture timed out after %d ms", HAL_CAMERA_CAPTURE_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    ret = s_ctx.capture_status;
    *image = s_ctx.capture_image;
    *image_size = s_ctx.capture_image_size;
    s_ctx.capture_image = NULL;
    s_ctx.capture_image_size = 0;
    s_ctx.capture_in_progress = false;
    xSemaphoreGive(s_ctx.lock);

    return ret;
}

static esp_err_t hal_camera_capture_jpeg_internal(bool from_stream, uint8_t **jpeg, size_t *jpeg_size,
                                                  uint32_t *timestamp_ms) {
    esp_err_t ret;
    char *image = NULL;
    int image_size = 0;

    ESP_RETURN_ON_FALSE(jpeg != NULL && jpeg_size != NULL && timestamp_ms != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "invalid jpeg output");

    *jpeg = NULL;
    *jpeg_size = 0;
    *timestamp_ms = 0;

    ret = hal_camera_take_image_string(from_stream, &image, &image_size);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "capture failed");
    ESP_GOTO_ON_FALSE(image != NULL && image_size > 0, ESP_ERR_INVALID_RESPONSE, cleanup, TAG, "capture image missing");
    ESP_GOTO_ON_ERROR(hal_camera_decode_image(image, jpeg, jpeg_size), cleanup, TAG, "image decode failed");

    *timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

cleanup:
    if (image != NULL) {
        free(image);
    }
    return ret;
}

static void hal_camera_stream_task(void *arg) {
    (void)arg;

    while (true) {
        uint64_t loop_start_us = (uint64_t)esp_timer_get_time();
        hal_camera_frame_cb_t frame_cb = NULL;
        void *frame_ctx = NULL;
        int fps = 0;
        uint8_t *jpeg = NULL;
        size_t jpeg_size = 0;
        uint32_t timestamp_ms = 0;
        esp_err_t ret;
        uint32_t ok_count = 0;
        uint32_t err_count = 0;

        if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) != pdTRUE) {
            break;
        }

        if (s_ctx.stream_stop_requested) {
            s_ctx.streaming = false;
            s_ctx.stream_stop_requested = false;
            s_ctx.stream_task = NULL;
            s_ctx.stream_cb = NULL;
            s_ctx.stream_ctx = NULL;
            s_ctx.stream_fps = 0;
            xSemaphoreGive(s_ctx.lock);
            break;
        }

        frame_cb = s_ctx.stream_cb;
        frame_ctx = s_ctx.stream_ctx;
        fps = s_ctx.stream_fps;
        xSemaphoreGive(s_ctx.lock);

        ret = hal_camera_capture_jpeg_internal(true, &jpeg, &jpeg_size, &timestamp_ms);
        if (ret == ESP_OK) {
            if (frame_cb != NULL) {
                frame_cb(jpeg, jpeg_size, timestamp_ms, frame_ctx);
            }

            if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) == pdTRUE) {
                s_ctx.stream_frames_ok++;
                ok_count = s_ctx.stream_frames_ok;
                xSemaphoreGive(s_ctx.lock);
            }

            if (ok_count > 0 && (ok_count % HAL_CAMERA_STREAM_LOG_EVERY) == 0) {
                ESP_LOGI(TAG, "stream frames ok=%lu err=%lu fps=%d last_jpeg=%u",
                         (unsigned long)ok_count,
                         (unsigned long)s_ctx.stream_frames_err,
                         fps,
                         (unsigned int)jpeg_size);
            }
        } else {
            if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) == pdTRUE) {
                s_ctx.stream_frames_err++;
                err_count = s_ctx.stream_frames_err;
                xSemaphoreGive(s_ctx.lock);
            }
            ESP_LOGW(TAG, "stream frame failed #%lu: %s", (unsigned long)err_count, esp_err_to_name(ret));
        }

        if (jpeg != NULL) {
            free(jpeg);
        }

        if (fps > 0) {
            uint64_t period_us = 1000000ULL / (uint64_t)fps;
            uint64_t elapsed_us = (uint64_t)esp_timer_get_time() - loop_start_us;
            if (elapsed_us < period_us) {
                uint32_t delay_ms = (uint32_t)((period_us - elapsed_us + 999ULL) / 1000ULL);
                if (delay_ms > 0) {
                    vTaskDelay(pdMS_TO_TICKS(delay_ms));
                }
            }
        }
    }

    ESP_LOGI(TAG, "camera stream task exited");
    vTaskDelete(NULL);
}

esp_err_t hal_camera_init(void) {
    esp_err_t ret;
    bool need_register = false;
    bool need_client_start = false;
    TickType_t waited_ticks = 0;
    const TickType_t step_ticks = pdMS_TO_TICKS(20);
    const TickType_t timeout_ticks = pdMS_TO_TICKS(HAL_CAMERA_CONNECT_TIMEOUT_MS);
    const sscma_client_callback_t callbacks = {
        .on_connect = hal_camera_on_connect,
        .on_disconnect = NULL,
        .on_response = hal_camera_on_response,
        .on_event = hal_camera_on_event,
        .on_log = NULL,
    };

    ESP_RETURN_ON_ERROR(hal_camera_ensure_sync_primitives(), TAG, "camera sync primitive init failed");

    while (true) {
        if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) != pdTRUE) {
            return ESP_FAIL;
        }

        if (s_ctx.initialized && s_ctx.connected && s_ctx.client != NULL) {
            xSemaphoreGive(s_ctx.lock);
            return ESP_OK;
        }

        if (s_ctx.init_in_progress) {
            esp_err_t init_status = s_ctx.init_status;
            bool initialized = s_ctx.initialized;
            xSemaphoreGive(s_ctx.lock);

            if (initialized && init_status == ESP_OK) {
                return ESP_OK;
            }

            if (waited_ticks >= timeout_ticks) {
                ESP_LOGE(TAG, "HX6538 init wait timeout after %d ms", HAL_CAMERA_CONNECT_TIMEOUT_MS);
                return ESP_ERR_TIMEOUT;
            }

            vTaskDelay(step_ticks);
            waited_ticks += step_ticks;
            continue;
        }

        s_ctx.init_in_progress = true;
        s_ctx.init_status = ESP_ERR_TIMEOUT;

        if (s_ctx.client == NULL) {
            s_ctx.client = bsp_sscma_client_init();
            if (s_ctx.client == NULL) {
                s_ctx.init_in_progress = false;
                s_ctx.init_status = ESP_FAIL;
                xSemaphoreGive(s_ctx.lock);
                ESP_LOGE(TAG, "bsp_sscma_client_init failed");
                return ESP_FAIL;
            }
        }

        need_register = !s_ctx.callbacks_registered;
        need_client_start = !s_ctx.client_started || !s_ctx.connected;
        s_ctx.connected = false;
        hal_camera_clear_capture_locked();
        xSemaphoreGive(s_ctx.lock);
        break;
    }

    if (need_register) {
        ret = sscma_client_register_callback(s_ctx.client, &callbacks, NULL);
        if (ret != ESP_OK) {
            if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) == pdTRUE) {
                s_ctx.init_in_progress = false;
                s_ctx.init_status = ret;
                xSemaphoreGive(s_ctx.lock);
            }
            ESP_LOGE(TAG, "sscma register callback failed: %s", esp_err_to_name(ret));
            return ret;
        }

        if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) == pdTRUE) {
            s_ctx.callbacks_registered = true;
            xSemaphoreGive(s_ctx.lock);
        }
    }

    if (need_client_start) {
        hal_camera_drain_semaphore(s_ctx.connect_sem);
        ret = sscma_client_init(s_ctx.client);
        if (ret != ESP_OK) {
            if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) == pdTRUE) {
                s_ctx.init_in_progress = false;
                s_ctx.init_status = ret;
                xSemaphoreGive(s_ctx.lock);
            }
            ESP_LOGE(TAG, "sscma init failed: %s", esp_err_to_name(ret));
            return ret;
        }

        if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) == pdTRUE) {
            s_ctx.client_started = true;
            xSemaphoreGive(s_ctx.lock);
        }

        ret = xSemaphoreTake(s_ctx.connect_sem, pdMS_TO_TICKS(HAL_CAMERA_CONNECT_TIMEOUT_MS)) == pdTRUE ? ESP_OK
                                                                                                         : ESP_ERR_TIMEOUT;
        if (ret != ESP_OK) {
            if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) == pdTRUE) {
                s_ctx.init_in_progress = false;
                s_ctx.init_status = ret;
                xSemaphoreGive(s_ctx.lock);
            }
            ESP_LOGE(TAG, "HX6538 connect timeout after %d ms", HAL_CAMERA_CONNECT_TIMEOUT_MS);
            return ret;
        }
    }

    if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) == pdTRUE) {
        s_ctx.initialized = true;
        s_ctx.init_in_progress = false;
        s_ctx.init_status = ESP_OK;
        xSemaphoreGive(s_ctx.lock);
    }

    hal_camera_log_device_info();
    return ESP_OK;
}

esp_err_t hal_camera_start(int fps, hal_camera_frame_cb_t cb, void *ctx) {
    BaseType_t task_ret;

    ESP_RETURN_ON_FALSE(cb != NULL, ESP_ERR_INVALID_ARG, TAG, "frame callback required");
    ESP_RETURN_ON_FALSE(fps > 0 && fps <= HAL_CAMERA_MAX_FPS, ESP_ERR_INVALID_ARG, TAG, "fps out of range");
    ESP_RETURN_ON_ERROR(hal_camera_init(), TAG, "hal_camera_init failed");

    if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    if (s_ctx.streaming || s_ctx.stream_task != NULL) {
        xSemaphoreGive(s_ctx.lock);
        ESP_LOGW(TAG, "camera already streaming");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ctx.capture_in_progress) {
        xSemaphoreGive(s_ctx.lock);
        ESP_LOGW(TAG, "camera busy with one-shot capture");
        return ESP_ERR_INVALID_STATE;
    }

    s_ctx.streaming = true;
    s_ctx.stream_stop_requested = false;
    s_ctx.stream_cb = cb;
    s_ctx.stream_ctx = ctx;
    s_ctx.stream_fps = fps;
    s_ctx.stream_frames_ok = 0;
    s_ctx.stream_frames_err = 0;
    xSemaphoreGive(s_ctx.lock);

    task_ret = xTaskCreate(hal_camera_stream_task, "hal_camera_stream", HAL_CAMERA_STREAM_TASK_STACK, NULL,
                           HAL_CAMERA_STREAM_TASK_PRIORITY, &s_ctx.stream_task);
    if (task_ret != pdPASS) {
        if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) == pdTRUE) {
            s_ctx.streaming = false;
            s_ctx.stream_cb = NULL;
            s_ctx.stream_ctx = NULL;
            s_ctx.stream_fps = 0;
            s_ctx.stream_task = NULL;
            xSemaphoreGive(s_ctx.lock);
        }
        ESP_LOGE(TAG, "failed to create stream task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "camera stream started, target_fps=%d", fps);
    return ESP_OK;
}

esp_err_t hal_camera_stop(void) {
    TickType_t waited_ticks = 0;
    const TickType_t step_ticks = pdMS_TO_TICKS(20);
    const TickType_t timeout_ticks = pdMS_TO_TICKS(HAL_CAMERA_STREAM_STOP_TIMEOUT_MS);

    if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    if (!s_ctx.streaming && s_ctx.stream_task == NULL) {
        xSemaphoreGive(s_ctx.lock);
        return ESP_OK;
    }

    s_ctx.stream_stop_requested = true;
    xSemaphoreGive(s_ctx.lock);

    while (waited_ticks < timeout_ticks) {
        bool done = false;

        if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) == pdTRUE) {
            done = (!s_ctx.streaming && s_ctx.stream_task == NULL);
            xSemaphoreGive(s_ctx.lock);
        }

        if (done) {
            ESP_LOGI(TAG, "camera stream stopped");
            return ESP_OK;
        }

        vTaskDelay(step_ticks);
        waited_ticks += step_ticks;
    }

    ESP_LOGW(TAG, "camera stream stop timed out after %d ms", HAL_CAMERA_STREAM_STOP_TIMEOUT_MS);
    return ESP_ERR_TIMEOUT;
}

esp_err_t hal_camera_capture_once(hal_camera_frame_cb_t cb, void *ctx) {
    uint8_t *jpeg = NULL;
    size_t jpeg_size = 0;
    uint32_t timestamp_ms;
    esp_err_t ret;

    ESP_RETURN_ON_FALSE(cb != NULL, ESP_ERR_INVALID_ARG, TAG, "frame callback required");
    ret = hal_camera_capture_jpeg_internal(false, &jpeg, &jpeg_size, &timestamp_ms);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "capture failed");
    cb(jpeg, jpeg_size, timestamp_ms, ctx);

cleanup:
    if (jpeg != NULL) {
        free(jpeg);
    }
    return ret;
}

bool hal_camera_is_streaming(void) {
    bool streaming = false;

    if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) == pdTRUE) {
        streaming = s_ctx.streaming;
        xSemaphoreGive(s_ctx.lock);
    }

    return streaming;
}
