#ifndef HAL_OPUS_H
#define HAL_OPUS_H

#include <stdint.h>

/**
 * @file hal_opus.h
 * @brief Audio codec HAL - PCM passthrough mode (no compression)
 *
 * MVP simplification: Direct PCM transmission without encoding.
 * Frame format: 16-bit, 16kHz, mono PCM.
 *
 * Future: Can be upgraded to Opus codec when needed.
 */

/**
 * Initialize audio codec
 * @return 0 on success, -1 on error
 */
int hal_opus_init(void);

/**
 * Process audio for transmission (passthrough - no encoding)
 * @param pcm_in Input PCM data (16-bit, 16kHz, mono)
 * @param pcm_len Length of PCM data in bytes
 * @param out_buf Output buffer
 * @param out_max_len Max output buffer size
 * @return Output bytes on success, -1 on error
 *
 * Note: In PCM mode, this is a simple memcpy passthrough.
 */
int hal_opus_encode(const uint8_t *pcm_in, int pcm_len,
                    uint8_t *out_buf, int out_max_len);

/**
 * Process received audio for playback (passthrough - no decoding)
 * @param in_data Input audio data
 * @param in_len Length of input data in bytes
 * @param pcm_out Output buffer for PCM data
 * @param pcm_max_len Max output buffer size
 * @return Output bytes on success, -1 on error
 *
 * Note: In PCM mode, this is a simple memcpy passthrough.
 */
int hal_opus_decode(const uint8_t *in_data, int in_len,
                    uint8_t *pcm_out, int pcm_max_len);

#endif /* HAL_OPUS_H */
