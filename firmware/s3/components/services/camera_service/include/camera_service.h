/**
 * @file camera_service.h
 * @brief Camera service over HAL camera
 *
 * Current implementation supports HAL initialization, sensor profile configuration,
 * one-shot capture, and continuous streaming.
 */

#ifndef CAMERA_SERVICE_H
#define CAMERA_SERVICE_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*camera_service_frame_cb_t)(const uint8_t *jpeg, size_t size, uint32_t timestamp_ms, void *ctx);

esp_err_t camera_service_init(void);
esp_err_t camera_service_configure(int width, int height, int quality, int *applied_width, int *applied_height);
esp_err_t camera_service_register_frame_callback(camera_service_frame_cb_t cb, void *ctx);
esp_err_t camera_service_start_stream(int fps);
esp_err_t camera_service_stop_stream(void);
esp_err_t camera_service_capture_once(void);
bool camera_service_is_streaming(void);

#endif /* CAMERA_SERVICE_H */
