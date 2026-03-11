/**
 * @file camera_service.c
 * @brief Camera service stub — Phase 7 implementation pending
 */

#include "camera_service.h"
#include "esp_log.h"

#define TAG "CAMERA_SVC"

esp_err_t camera_service_init(void)
{
    ESP_LOGW(TAG, "camera_service_init: Phase 7 stub");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t camera_service_start_stream(int fps)
{
    ESP_LOGW(TAG, "camera_service_start_stream(%d): stub", fps);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t camera_service_stop_stream(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t camera_service_capture_once(void)
{
    ESP_LOGW(TAG, "camera_service_capture_once: stub");
    return ESP_ERR_NOT_SUPPORTED;
}
