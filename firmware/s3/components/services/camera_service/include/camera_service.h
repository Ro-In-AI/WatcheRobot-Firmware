/**
 * @file camera_service.h
 * @brief Camera video streaming service (Phase 7)
 *
 * Schedules JPEG frame streaming over WebSocket.
 * Handles bandwidth priority (TTS > audio > video).
 */

#ifndef CAMERA_SERVICE_H
#define CAMERA_SERVICE_H

#include "esp_err.h"

esp_err_t camera_service_init(void);
esp_err_t camera_service_start_stream(int fps);
esp_err_t camera_service_stop_stream(void);
esp_err_t camera_service_capture_once(void);
bool camera_service_is_streaming(void);

#endif /* CAMERA_SERVICE_H */
