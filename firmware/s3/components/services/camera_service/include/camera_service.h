/**
 * @file camera_service.h
 * @brief Camera service over HAL camera
 *
 * Current implementation supports HAL initialization and one-shot capture.
 * Streaming remains deferred until the HAL stream path is implemented.
 */

#ifndef CAMERA_SERVICE_H
#define CAMERA_SERVICE_H

#include "esp_err.h"
#include <stdbool.h>

esp_err_t camera_service_init(void);
esp_err_t camera_service_start_stream(int fps);
esp_err_t camera_service_stop_stream(void);
esp_err_t camera_service_capture_once(void);
bool camera_service_is_streaming(void);

#endif /* CAMERA_SERVICE_H */
