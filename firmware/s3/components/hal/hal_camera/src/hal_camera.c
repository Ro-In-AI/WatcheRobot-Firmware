/**
 * @file hal_camera.c
 * @brief Camera HAL stub — Phase 7 implementation pending
 *
 * Phase 1: returns ESP_ERR_NOT_SUPPORTED.
 * Phase 7: implement SSCMA client wrapper for Himax HX6538.
 */

#include "hal_camera.h"
#include "esp_log.h"

#define TAG "HAL_CAMERA"

esp_err_t hal_camera_init(void) {
    ESP_LOGW(TAG, "hal_camera_init: Phase 7 stub");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t hal_camera_start(int fps, hal_camera_frame_cb_t cb, void *ctx) {
    ESP_LOGW(TAG, "hal_camera_start(%d fps): stub", fps);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t hal_camera_stop(void) {
    ESP_LOGW(TAG, "hal_camera_stop: stub");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t hal_camera_capture_once(hal_camera_frame_cb_t cb, void *ctx) {
    ESP_LOGW(TAG, "hal_camera_capture_once: stub");
    return ESP_ERR_NOT_SUPPORTED;
}

bool hal_camera_is_streaming(void) {
    return false;
}
