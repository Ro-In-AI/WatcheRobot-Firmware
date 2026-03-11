/**
 * @file hal_wake_word.h
 * @brief Wake Word Detection Hardware Abstraction Layer
 *
 * This HAL provides an interface for offline wake word detection using ESP-SR.
 * The implementation supports:
 * - Pre-trained wake words (e.g., "Ni Hao Xiao Zhi")
 * - Custom wake words via Multinet (pinyin-based)
 *
 * Architecture:
 *   [Microphone] → I2S → hal_audio_read() → hal_wake_word_feed()
 *                                              ↓
 *                                        [ESP-SR AFE]
 *                                              ↓
 *                                    Wake Word Detected?
 *                                              ↓
 *                                       callback()
 */

#ifndef HAL_WAKE_WORD_H
#define HAL_WAKE_WORD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Opaque Types                                                       */
/* ------------------------------------------------------------------ */

/**
 * Opaque wake word detection context
 */
typedef struct wake_word_ctx_s wake_word_ctx_t;

/* ------------------------------------------------------------------ */
/* Callback Types                                                     */
/* ------------------------------------------------------------------ */

/**
 * Wake word detection callback
 *
 * @param wake_word The detected wake word string (e.g., "nihaoxiaozhi")
 * @param user_data User-provided context pointer
 */
typedef void (*wake_word_callback_t)(const char *wake_word, void *user_data);

/* ------------------------------------------------------------------ */
/* Configuration                                                      */
/* ------------------------------------------------------------------ */

/**
 * Wake word detector configuration
 */
typedef struct {
    const char *model_path;           /*!< SPIFFS path to model directory (e.g., "/spiffs/model") */
    const char *wake_word_phrase;     /*!< Custom wake word phrase (pinyin, for multinet mode) */
    float detection_threshold;        /*!< Detection threshold (0.0-1.0), lower = more sensitive */
    wake_word_callback_t callback;    /*!< Callback when wake word is detected */
    void *user_data;                  /*!< User data passed to callback */
} wake_word_config_t;

/* ------------------------------------------------------------------ */
/* Core API                                                           */
/* ------------------------------------------------------------------ */

/**
 * Initialize wake word detector
 *
 * This initializes the ESP-SR AFE (Audio Front-End) engine for wake word
 * detection. Requires PSRAM to be enabled.
 *
 * @param config Configuration parameters
 * @return Context handle on success, NULL on failure
 */
wake_word_ctx_t *hal_wake_word_init(const wake_word_config_t *config);

/**
 * Feed audio samples to wake word detector
 *
 * Call this function with microphone data when in IDLE state.
 * The detector processes the audio locally (offline) and triggers
 * the callback if a wake word is detected.
 *
 * @param ctx Context handle from hal_wake_word_init()
 * @param samples 16-bit PCM samples (16kHz, mono)
 * @param num_samples Number of samples (not bytes!)
 */
void hal_wake_word_feed(wake_word_ctx_t *ctx, const int16_t *samples, size_t num_samples);

/**
 * Start wake word detection
 *
 * After calling this, the detector will process audio fed via hal_wake_word_feed()
 * and trigger the callback when a wake word is detected.
 *
 * @param ctx Context handle
 */
void hal_wake_word_start(wake_word_ctx_t *ctx);

/**
 * Stop wake word detection
 *
 * Temporarily disables detection. Audio fed during this time is ignored.
 * Call hal_wake_word_start() to resume detection.
 *
 * @param ctx Context handle
 */
void hal_wake_word_stop(wake_word_ctx_t *ctx);

/**
 * Get required feed size
 *
 * Returns the optimal number of samples to feed per hal_wake_word_feed() call.
 * Feeding this exact size ensures optimal performance.
 *
 * @param ctx Context handle
 * @return Number of samples per feed, or 0 if not initialized
 */
size_t hal_wake_word_get_feed_size(wake_word_ctx_t *ctx);

/**
 * Destroy wake word detector and free resources
 *
 * @param ctx Context handle (may be NULL)
 */
void hal_wake_word_deinit(wake_word_ctx_t *ctx);

/* ------------------------------------------------------------------ */
/* Utility Functions                                                  */
/* ------------------------------------------------------------------ */

/**
 * Check if wake word detection is supported on this hardware
 *
 * @return true if ESP-SR is available and PSRAM is enabled
 */
bool hal_wake_word_is_supported(void);

/**
 * Get the last detected wake word
 *
 * @param ctx Context handle
 * @return Wake word string, or NULL if none detected yet
 */
const char *hal_wake_word_get_last_detected(wake_word_ctx_t *ctx);

/**
 * Get list of available wake words
 *
 * @param ctx Context handle
 * @param out_buf Buffer to store wake word list (semicolon-separated)
 * @param buf_size Size of output buffer
 * @return Number of characters written, or 0 on error
 */
int hal_wake_word_get_available_list(wake_word_ctx_t *ctx, char *out_buf, size_t buf_size);

#endif /* HAL_WAKE_WORD_H */
