#ifndef HAL_AUDIO_H
#define HAL_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Initialize audio codec (call once at startup)
 * This initializes ES7243 ADC and ES8311 DAC via I2C
 */
int hal_audio_init(void);

/**
 * Start audio capture/playback
 */
int hal_audio_start(void);

/**
 * Read audio samples from microphone
 * @param out_buf Output buffer
 * @param max_len Maximum length
 * @return Number of bytes read, or -1 on error
 */
int hal_audio_read(uint8_t *out_buf, int max_len);

/**
 * Write audio samples to speaker
 * @param data Audio data
 * @param len Data length
 * @return Number of bytes written, or -1 on error
 */
int hal_audio_write(const uint8_t *data, int len);

/**
 * Stop audio capture/playback
 */
int hal_audio_stop(void);

/**
 * Set sample rate for playback (call before TTS playback)
 * @param sample_rate Sample rate in Hz (e.g., 16000, 24000)
 */
void hal_audio_set_sample_rate(uint32_t sample_rate);

/**
 * Mark audio as being used for playback (not just recording)
 * @param enable true for TTS playback mode, false for recording mode
 */
void hal_audio_set_playback_mode(bool enable);

#endif /* HAL_AUDIO_H */
