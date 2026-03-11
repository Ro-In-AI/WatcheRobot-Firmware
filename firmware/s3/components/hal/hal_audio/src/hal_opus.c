#include "hal_opus.h"
#include "esp_log.h"
#include <string.h>

#define TAG "HAL_PCM"

/**
 * @file hal_opus.c
 * @brief Audio codec HAL - PCM passthrough implementation
 *
 * MVP simplification: Direct PCM transmission without any encoding.
 * This reduces complexity and CPU usage on the ESP32-S3.
 *
 * Trade-offs:
 * - Pros: Simple implementation, no CPU overhead, no codec library dependency
 * - Cons: Higher bandwidth (~10x compared to Opus), no noise reduction
 *
 * PCM format: 16-bit signed, 16kHz sample rate, mono
 * Frame size: 60ms = 960 samples = 1920 bytes
 * Bandwidth: ~256 kbps (vs ~24 kbps with Opus)
 */

int hal_opus_init(void)
{
    ESP_LOGI(TAG, "Audio codec initialized (PCM passthrough mode)");
    return 0;
}

int hal_opus_encode(const uint8_t *pcm_in, int pcm_len,
                    uint8_t *out_buf, int out_max_len)
{
    if (!pcm_in || !out_buf || pcm_len <= 0 || out_max_len <= 0) {
        return -1;
    }

    /* PCM passthrough: just copy the data */
    int out_len = (pcm_len < out_max_len) ? pcm_len : out_max_len;
    memcpy(out_buf, pcm_in, out_len);

    ESP_LOGD(TAG, "PCM passthrough: %d bytes", out_len);
    return out_len;
}

int hal_opus_decode(const uint8_t *in_data, int in_len,
                    uint8_t *pcm_out, int pcm_max_len)
{
    if (!in_data || !pcm_out || in_len <= 0 || pcm_max_len <= 0) {
        return -1;
    }

    /* PCM passthrough: just copy the data */
    int out_len = (in_len < pcm_max_len) ? in_len : pcm_max_len;
    memcpy(pcm_out, in_data, out_len);

    ESP_LOGD(TAG, "PCM passthrough: %d bytes", out_len);
    return out_len;
}
