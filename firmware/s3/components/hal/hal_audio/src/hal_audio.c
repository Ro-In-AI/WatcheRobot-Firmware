#include "hal_audio.h"
#include "sensecap-watcher.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "HAL_AUDIO"

/* Default sample rates */
#define SAMPLE_RATE_RECORD  16000   /* ASR expects 16kHz */
#define SAMPLE_RATE_PLAY    24000   /* 火山引擎 TTS uses 24kHz */

static bool codec_initialized = false;  /* codec init is global, only once */
static bool is_running = false;         /* current running state */
static uint32_t current_sample_rate = SAMPLE_RATE_RECORD;  /* current sample rate */
static esp_codec_dev_handle_t mic_handle = NULL;
static esp_codec_dev_handle_t speaker_handle = NULL;

/* Initialize codec once at system startup */
int hal_audio_init(void)
{
    if (codec_initialized) {
        return 0;
    }

    ESP_LOGI(TAG, "Initializing audio codec via SDK...");

    esp_err_t ret = bsp_codec_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init codec: %s", esp_err_to_name(ret));
        return -1;
    }

    /* Get microphone and speaker handles */
    mic_handle = bsp_codec_microphone_get();
    speaker_handle = bsp_codec_speaker_get();

    if (!mic_handle) {
        ESP_LOGE(TAG, "Failed to get microphone handle");
        return -1;
    }

    if (!speaker_handle) {
        ESP_LOGE(TAG, "Failed to get speaker handle");
        return -1;
    }

    codec_initialized = true;
    is_running = true;  /* Keep codec running always */

    /* Set default sample rate for recording (16kHz) */
    bsp_codec_set_fs(SAMPLE_RATE_RECORD, 16, 1);
    current_sample_rate = SAMPLE_RATE_RECORD;

    /* Set volume and unmute (required for speaker output) */
    /* NOTE: Volume 100 can cause clipping distortion, use 80 for cleaner output */
    bsp_codec_mute_set(false);
    bsp_codec_volume_set(80, NULL);

    ESP_LOGI(TAG, "Audio codec initialized (16kHz for recording, volume=100)");
    return 0;
}

/* Track audio mode: recording (input) or playback (output) */
static bool is_playback_mode = false;

/* Set sample rate for playback (call before TTS playback) */
void hal_audio_set_sample_rate(uint32_t sample_rate)
{
    if (!codec_initialized) {
        return;
    }

    if (current_sample_rate == sample_rate) {
        return;  /* No change needed */
    }

    ESP_LOGI(TAG, "Switching sample rate: %lu -> %lu (playback_mode=%d)",
             current_sample_rate, sample_rate, is_playback_mode);

    /* For playback mode, we don't need to stop input channel (which may not be enabled).
     * Just set the new sample rate directly. */
    if (is_playback_mode) {
        /* Set new sample rate (16-bit, mono channel) */
        bsp_codec_set_fs(sample_rate, 16, 1);
        current_sample_rate = sample_rate;
        ESP_LOGI(TAG, "Sample rate switch complete (playback mode, no stop needed)");
        return;
    }

    /* For recording mode (input only), we need to reconfigure I2S.
     * Use bsp_codec_set_fs which handles I2S reconfiguration properly. */
    bsp_codec_set_fs(sample_rate, 16, 1);
    current_sample_rate = sample_rate;

    ESP_LOGI(TAG, "Sample rate switch complete");
}

/* Mark audio as being used for playback (not just recording) */
void hal_audio_set_playback_mode(bool enable)
{
    is_playback_mode = enable;
    ESP_LOGI(TAG, "Audio mode: %s", enable ? "playback" : "recording");
}

int hal_audio_start(void)
{
    if (is_running) {
        return 0;
    }

    /* Ensure codec is initialized */
    if (!codec_initialized) {
        if (hal_audio_init() != 0) {
            return -1;
        }
    }

    /* NOTE: Don't call bsp_codec_dev_resume() here because it uses
     * hardcoded DRV_AUDIO_SAMPLE_RATE (16kHz), which would override
     * the sample rate we just set in hal_audio_set_sample_rate().
     * The sample rate is already configured correctly.
     */

    is_running = true;
    ESP_LOGI(TAG, "Audio started (sample rate: %lu Hz)", current_sample_rate);
    return 0;
}

int hal_audio_read(uint8_t *out_buf, int max_len)
{
    if (!mic_handle) {
        return -1;
    }

    /* In wake word mode, audio should always be available */
    /* If is_running is false, return 0 (no data) instead of -1 (error) to avoid error spam */
    if (!is_running) {
#ifdef CONFIG_ENABLE_WAKE_WORD
        return 0;  /* In wake word mode, gracefully handle temporary unavailability */
#else
        return -1;
#endif
    }

    size_t bytes_read = 0;
    esp_err_t ret = bsp_i2s_read(out_buf, max_len, &bytes_read, 100);

    if (ret != ESP_OK) {
#ifdef CONFIG_ENABLE_WAKE_WORD
        /* In wake word mode, temporary read errors are expected during sample rate switches */
        ESP_LOGD(TAG, "Read temporarily unavailable: %s", esp_err_to_name(ret));
        return 0;
#else
        ESP_LOGE(TAG, "Read error: %s", esp_err_to_name(ret));
        return -1;
#endif
    }

    return (int)bytes_read;
}

int hal_audio_write(const uint8_t *data, int len)
{
    if (!is_running || !speaker_handle) {
        ESP_LOGW(TAG, "Write blocked: is_running=%d, speaker_handle=%p", is_running, speaker_handle);
        return -1;
    }

    size_t bytes_written = 0;
    /* Use ESP_LOGD for high-frequency audio writes to avoid UART bottleneck */
    ESP_LOGD(TAG, "Writing %d bytes to speaker...", len);
    esp_err_t ret = bsp_i2s_write((void *)data, len, &bytes_written, 100);
    ESP_LOGD(TAG, "Write result: ret=%d, written=%d", ret, (int)bytes_written);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Write error: %s", esp_err_to_name(ret));
        return -1;
    }

    return (int)bytes_written;
}

int hal_audio_stop(void)
{
    if (!is_running) {
        return 0;
    }

    /* Just mark as stopped, don't actually stop the codec */
    /* This avoids I2S reconfiguration issues */
    is_running = false;
    ESP_LOGI(TAG, "Audio stopped (codec stays running)");
    return 0;
}
