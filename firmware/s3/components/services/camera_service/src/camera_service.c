#include "camera_service.h"

#include "esp_check.h"
#include "esp_log.h"
#include "hal_camera.h"

#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "CAMERA_SVC"
#define CAMERA_SERVICE_LOG_EVERY 30

typedef struct {
    SemaphoreHandle_t lock;
    bool initialized;
    bool streaming;
    size_t last_frame_size;
    uint32_t last_timestamp_ms;
    uint32_t capture_count;
    camera_service_frame_cb_t frame_cb;
    void *frame_ctx;
} camera_service_context_t;

static camera_service_context_t s_ctx = {
    .lock = NULL,
    .initialized = false,
    .streaming = false,
    .last_frame_size = 0,
    .last_timestamp_ms = 0,
    .capture_count = 0,
    .frame_cb = NULL,
    .frame_ctx = NULL,
};

static esp_err_t camera_service_ensure_lock(void) {
    if (s_ctx.lock == NULL) {
        s_ctx.lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_ctx.lock != NULL, ESP_ERR_NO_MEM, TAG, "camera service lock create failed");
    }

    return ESP_OK;
}

static void camera_service_frame_cb_internal(const uint8_t *jpeg, size_t size, uint32_t timestamp_ms, void *ctx) {
    camera_service_context_t *service = (camera_service_context_t *)ctx;
    camera_service_frame_cb_t user_cb = NULL;
    void *user_ctx = NULL;

    if (service == NULL || jpeg == NULL || size == 0) {
        return;
    }

    if (service->lock != NULL && xSemaphoreTake(service->lock, portMAX_DELAY) == pdTRUE) {
        service->last_frame_size = size;
        service->last_timestamp_ms = timestamp_ms;
        service->capture_count++;
        user_cb = service->frame_cb;
        user_ctx = service->frame_ctx;
        xSemaphoreGive(service->lock);
    }

    if (service->capture_count == 1 || (service->capture_count % CAMERA_SERVICE_LOG_EVERY) == 0) {
        ESP_LOGI(TAG, "frame #%lu ok, jpeg=%u bytes ts=%lu ms streaming=%s",
                 (unsigned long)service->capture_count,
                 (unsigned int)size,
                 (unsigned long)timestamp_ms,
                 service->streaming ? "true" : "false");
    }

    if (user_cb != NULL) {
        user_cb(jpeg, size, timestamp_ms, user_ctx);
    }
}

esp_err_t camera_service_init(void) {
    ESP_RETURN_ON_ERROR(camera_service_ensure_lock(), TAG, "camera service lock init failed");

    if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    if (s_ctx.initialized) {
        xSemaphoreGive(s_ctx.lock);
        return ESP_OK;
    }

    xSemaphoreGive(s_ctx.lock);
    ESP_RETURN_ON_ERROR(hal_camera_init(), TAG, "hal_camera_init failed");

    if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    if (!s_ctx.initialized) {
        s_ctx.initialized = true;
        ESP_LOGI(TAG, "camera service initialized");
    }

    xSemaphoreGive(s_ctx.lock);
    return ESP_OK;
}

esp_err_t camera_service_register_frame_callback(camera_service_frame_cb_t cb, void *ctx) {
    ESP_RETURN_ON_ERROR(camera_service_ensure_lock(), TAG, "camera service lock init failed");

    if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    s_ctx.frame_cb = cb;
    s_ctx.frame_ctx = ctx;
    xSemaphoreGive(s_ctx.lock);
    return ESP_OK;
}

esp_err_t camera_service_start_stream(int fps) {
    ESP_RETURN_ON_ERROR(camera_service_init(), TAG, "camera_service_init failed");

    if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    if (s_ctx.streaming) {
        xSemaphoreGive(s_ctx.lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_ctx.streaming = true;
    xSemaphoreGive(s_ctx.lock);

    if (hal_camera_start(fps, camera_service_frame_cb_internal, &s_ctx) != ESP_OK) {
        if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) == pdTRUE) {
            s_ctx.streaming = false;
            xSemaphoreGive(s_ctx.lock);
        }
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "camera stream service started, target_fps=%d", fps);
    return ESP_OK;
}

esp_err_t camera_service_stop_stream(void) {
    esp_err_t ret = hal_camera_stop();

    if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) == pdTRUE) {
        s_ctx.streaming = false;
        xSemaphoreGive(s_ctx.lock);
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "camera stream service stopped");
    }

    return ret;
}

esp_err_t camera_service_capture_once(void) {
    ESP_RETURN_ON_ERROR(camera_service_init(), TAG, "camera_service_init failed");
    return hal_camera_capture_once(camera_service_frame_cb_internal, &s_ctx);
}

bool camera_service_is_streaming(void) {
    return hal_camera_is_streaming();
}
