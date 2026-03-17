#include "camera_service.h"

#include "esp_check.h"
#include "esp_log.h"
#include "hal_camera.h"

#include <stddef.h>
#include <stdint.h>

#define TAG "CAMERA_SVC"

typedef struct {
    bool initialized;
    size_t last_frame_size;
    uint32_t last_timestamp_ms;
    uint32_t capture_count;
} camera_service_context_t;

static camera_service_context_t s_ctx = {
    .initialized = false,
    .last_frame_size = 0,
    .last_timestamp_ms = 0,
    .capture_count = 0,
};

static void camera_service_capture_cb(const uint8_t *jpeg, size_t size, uint32_t timestamp_ms, void *ctx) {
    camera_service_context_t *service = (camera_service_context_t *)ctx;

    if (service == NULL || jpeg == NULL || size == 0) {
        return;
    }

    service->last_frame_size = size;
    service->last_timestamp_ms = timestamp_ms;
    service->capture_count++;

    ESP_LOGI(TAG, "capture #%lu ok, jpeg=%u bytes ts=%lu ms",
             (unsigned long)service->capture_count,
             (unsigned int)size,
             (unsigned long)timestamp_ms);
}

esp_err_t camera_service_init(void) {
    if (s_ctx.initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(hal_camera_init(), TAG, "hal_camera_init failed");
    s_ctx.initialized = true;
    ESP_LOGI(TAG, "camera service initialized");
    return ESP_OK;
}

esp_err_t camera_service_start_stream(int fps) {
    ESP_LOGW(TAG, "camera_service_start_stream(%d): streaming deferred", fps);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t camera_service_stop_stream(void) {
    ESP_LOGW(TAG, "camera_service_stop_stream: streaming deferred");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t camera_service_capture_once(void) {
    ESP_RETURN_ON_ERROR(camera_service_init(), TAG, "camera_service_init failed");
    return hal_camera_capture_once(camera_service_capture_cb, &s_ctx);
}

bool camera_service_is_streaming(void) {
    return hal_camera_is_streaming();
}
