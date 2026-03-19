/**
 * @file hal_camera.h
 * @brief Camera HAL: SSCMA client wrapper for Himax HX6538 vision AI chip
 *
 * Provides JPEG frame callback interface via SSCMA protocol.
 * Himax chip communicates with ESP32-S3 via UART/SPI SSCMA protocol.
 *
 * Current implementation supports:
 * - SSCMA client initialization and connect wait
 * - one-shot frame capture via hal_camera_capture_once()
 * - continuous frame capture via hal_camera_start()/hal_camera_stop()
 *
 * Streaming is currently implemented as a paced one-shot INVOKE loop over SSCMA.
 */

#ifndef HAL_CAMERA_H
#define HAL_CAMERA_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief JPEG frame callback type
 *
 * Called from the camera task when a new JPEG frame is available.
 *
 * @param jpeg         Pointer to JPEG data (valid only during callback)
 * @param size         JPEG data size in bytes
 * @param timestamp_ms Capture timestamp in milliseconds
 * @param ctx          User context pointer
 */
typedef void (*hal_camera_frame_cb_t)(const uint8_t *jpeg, size_t size, uint32_t timestamp_ms, void *ctx);

/**
 * @brief Initialize camera HAL (SSCMA client setup).
 *
 * @return ESP_OK on success, ESP_FAIL if SSCMA initialization fails
 */
esp_err_t hal_camera_init(void);

/**
 * @brief Configure the active HX6538 camera sensor output profile.
 *
 * Supported profiles currently map to the sensor catalog reported by the coprocessor:
 * - 240x240
 * - 416x416
 * - 480x480
 * - 640x480
 *
 * Quality is accepted as a hint for upper layers, but the current SSCMA path
 * does not expose a writable JPEG quality control.
 *
 * @param width          Requested output width
 * @param height         Requested output height
 * @param quality        Requested quality hint (stored for diagnostics only)
 * @param applied_width  Optional: applied width on success
 * @param applied_height Optional: applied height on success
 * @return ESP_OK on success
 */
esp_err_t hal_camera_configure(int width, int height, int quality, int *applied_width, int *applied_height);

/**
 * @brief Start continuous frame capture.
 *
 * @param fps Target frame rate (1–30). Actual rate depends on Himax AI workload.
 * @param cb  Frame callback (called from camera task context)
 * @param ctx User context passed to callback
 * @return ESP_OK on success
 */
esp_err_t hal_camera_start(int fps, hal_camera_frame_cb_t cb, void *ctx);

/**
 * @brief Stop continuous frame capture.
 *
 * @return ESP_OK on success
 */
esp_err_t hal_camera_stop(void);

/**
 * @brief Capture a single JPEG frame.
 *
 * @param cb  Frame callback (called once when frame is ready)
 * @param ctx User context passed to callback
 * @return ESP_OK on success
 */
esp_err_t hal_camera_capture_once(hal_camera_frame_cb_t cb, void *ctx);

/**
 * @brief Check if camera is currently streaming.
 *
 * @return true if streaming, false otherwise
 */
bool hal_camera_is_streaming(void);

#endif /* HAL_CAMERA_H */
