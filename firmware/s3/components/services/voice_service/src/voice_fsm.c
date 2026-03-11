#include "button_voice.h"
#include "hal_audio.h"
#include "hal_opus.h"
#include "hal_button.h"
#include "hal_wake_word.h"
#include "ws_client.h"
#include "display_ui.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>

#define TAG "VOICE"

/* ------------------------------------------------------------------ */
/* Private: Wake word context                                         */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_ENABLE_WAKE_WORD
static wake_word_ctx_t *g_wake_word_ctx = NULL;

/* Forward declarations */
static void on_wake_word_detected(const char *wake_word, void *user_data);
static int wake_word_setup(void);
static void wake_word_cleanup(void);
#endif /* CONFIG_ENABLE_WAKE_WORD */

/* ------------------------------------------------------------------ */
/* Private: State and statistics                                       */
/* ------------------------------------------------------------------ */

static voice_state_t g_state = VOICE_STATE_IDLE;
static voice_stats_t g_stats = {0};

/* Track how recording was triggered (for button behavior) */
static bool g_recording_triggered_by_wake_word = false;

/* Audio buffer for PCM data (16kHz, 16-bit, 60ms frame = 1920 bytes) */
#define PCM_FRAME_SIZE  1920

static uint8_t g_pcm_buf[PCM_FRAME_SIZE];

/* ------------------------------------------------------------------ */
/* Private: VAD (Voice Activity Detection)                             */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_ENABLE_WAKE_WORD
/* VAD state */
static int g_vad_silence_frames = 0;    /* Consecutive silent frames */
static int g_vad_speech_frames = 0;     /* Total speech frames in this recording */

/* VAD configuration from Kconfig */
#define VAD_FRAME_MS            60      /* Each frame is 60ms */
#define VAD_SILENCE_FRAMES      (CONFIG_VAD_SILENCE_TIMEOUT_MS / VAD_FRAME_MS)
#define VAD_RMS_THRESHOLD       CONFIG_VAD_RMS_THRESHOLD
#define VAD_MIN_SPEECH_FRAMES   (CONFIG_VAD_MIN_SPEECH_MS / VAD_FRAME_MS)

/* VAD control: only enable when wake word triggered */
static bool g_vad_enabled = false;

static void vad_reset(void)
{
    g_vad_silence_frames = 0;
    g_vad_speech_frames = 0;
    g_vad_enabled = true;
    ESP_LOGI(TAG, "VAD reset, silence_threshold=%d frames, rms_threshold=%d, min_speech=%d frames",
             VAD_SILENCE_FRAMES, VAD_RMS_THRESHOLD, VAD_MIN_SPEECH_FRAMES);
}

static void vad_disable(void)
{
    g_vad_enabled = false;
}

/**
 * Process VAD on a frame
 * @param rms RMS value of the audio frame
 * @return true if recording should stop (silence timeout)
 */
static bool vad_process_frame(int rms)
{
    if (!g_vad_enabled) {
        return false;
    }

    /* Skip VAD if silence timeout is disabled (0) */
    if (VAD_SILENCE_FRAMES <= 0) {
        return false;
    }

    if (rms < VAD_RMS_THRESHOLD) {
        /* Silent frame */
        g_vad_silence_frames++;

        /* Log every 10 silent frames */
        if (g_vad_silence_frames % 10 == 0) {
            ESP_LOGI(TAG, "VAD: silence_frames=%d/%d, rms=%d (threshold=%d)",
                     g_vad_silence_frames, VAD_SILENCE_FRAMES, rms, VAD_RMS_THRESHOLD);
        }

        /* Check if silence timeout reached and minimum speech achieved */
        if (g_vad_silence_frames >= VAD_SILENCE_FRAMES &&
            g_vad_speech_frames >= VAD_MIN_SPEECH_FRAMES) {
            ESP_LOGI(TAG, "VAD: Silence timeout detected! speech_frames=%d, silence_frames=%d",
                     g_vad_speech_frames, g_vad_silence_frames);
            return true;  /* Signal to stop recording */
        }
    } else {
        /* Speech frame */
        g_vad_silence_frames = 0;  /* Reset silence counter */
        g_vad_speech_frames++;

        /* Log every 20 speech frames */
        if (g_vad_speech_frames % 20 == 0) {
            ESP_LOGI(TAG, "VAD: speech_frames=%d, rms=%d", g_vad_speech_frames, rms);
        }
    }

    return false;
}
#endif /* CONFIG_ENABLE_WAKE_WORD */

/* ------------------------------------------------------------------ */
/* Public: Initialize                                                 */
/* ------------------------------------------------------------------ */

void voice_recorder_init(void)
{
    g_state = VOICE_STATE_IDLE;
    memset(&g_stats, 0, sizeof(g_stats));

#ifdef CONFIG_ENABLE_WAKE_WORD
    /* Start audio immediately for wake word detection */
    ESP_LOGI(TAG, "Wake word enabled, starting audio for continuous detection");
    if (hal_audio_start() != 0) {
        ESP_LOGE(TAG, "Failed to start audio for wake word detection");
    }
#endif
}

/* ------------------------------------------------------------------ */
/* Public: Get current state                                          */
/* ------------------------------------------------------------------ */

voice_state_t voice_recorder_get_state(void)
{
    return g_state;
}

/* ------------------------------------------------------------------ */
/* Public: Reset statistics                                           */
/* ------------------------------------------------------------------ */

void voice_recorder_reset_stats(void)
{
    g_stats.record_count = 0;
    g_stats.encode_count = 0;
    g_stats.error_count = 0;
}

/* ------------------------------------------------------------------ */
/* Public: Get statistics                                             */
/* ------------------------------------------------------------------ */

void voice_recorder_get_stats(voice_stats_t *out_stats)
{
    if (out_stats) {
        out_stats->record_count = g_stats.record_count;
        out_stats->encode_count = g_stats.encode_count;
        out_stats->error_count = g_stats.error_count;
        out_stats->current_state = (int)g_state;
    }
}

/* ------------------------------------------------------------------ */
/* Private: Start recording                                           */
/* ------------------------------------------------------------------ */

static int start_recording(void)
{
    ESP_LOGI(TAG, "start_recording: calling hal_audio_start()");
    if (hal_audio_start() != 0) {
        ESP_LOGE(TAG, "start_recording: hal_audio_start failed");
        g_stats.error_count++;
        return -1;
    }

#ifdef CONFIG_ENABLE_WAKE_WORD
    /* Stop wake word detection during recording to prevent AFE empty warnings */
    if (g_wake_word_ctx != NULL) {
        hal_wake_word_stop(g_wake_word_ctx);
        /* Wait for detection task to finish current fetch */
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    /* Initialize VAD for wake word mode */
    if (g_recording_triggered_by_wake_word) {
        vad_reset();
        ESP_LOGI(TAG, "VAD enabled: silence_timeout=%dms, rms_threshold=%d, min_speech=%dms",
                 CONFIG_VAD_SILENCE_TIMEOUT_MS, CONFIG_VAD_RMS_THRESHOLD, CONFIG_VAD_MIN_SPEECH_MS);
    }
#endif

    g_state = VOICE_STATE_RECORDING;
    ESP_LOGI(TAG, "start_recording: state -> RECORDING");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Private: Stop recording                                            */
/* ------------------------------------------------------------------ */

static int stop_recording(void)
{
    /* In wake word mode, keep audio running for continuous detection */
#ifdef CONFIG_ENABLE_WAKE_WORD
    if (!g_recording_triggered_by_wake_word) {
        hal_audio_stop();
    }
    /* Wake word mode: audio stays running for next detection */
#else
    hal_audio_stop();
#endif

    /* Send audio end marker */
    if (ws_send_audio_end() != 0) {
        g_stats.error_count++;
        /* Still transition to idle */
    }

#ifdef CONFIG_ENABLE_WAKE_WORD
    /* Disable VAD when stopping */
    vad_disable();
#endif

    g_state = VOICE_STATE_IDLE;
    g_stats.record_count++;
    g_recording_triggered_by_wake_word = false;  /* Reset trigger flag */
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public: Process event                                              */
/* ------------------------------------------------------------------ */

void voice_recorder_process_event(voice_event_t event)
{
    switch (g_state) {
        case VOICE_STATE_IDLE:
            if (event == VOICE_EVENT_BUTTON_PRESS ||
                event == VOICE_EVENT_WAKE_WORD) {
#ifdef CONFIG_ENABLE_WAKE_WORD
                if (event == VOICE_EVENT_WAKE_WORD) {
                    ESP_LOGI(TAG, "Wake word triggered recording");
                }
#endif
                start_recording();
            }
            break;

        case VOICE_STATE_RECORDING:
            if (event == VOICE_EVENT_BUTTON_RELEASE ||
                event == VOICE_EVENT_TIMEOUT) {
                stop_recording();
            }
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Public: Process tick (read, send)                                   */
/* ------------------------------------------------------------------ */

int voice_recorder_tick(void)
{
    /* Always read audio when wake word detection is enabled */
    int pcm_len = 0;

#ifdef CONFIG_ENABLE_WAKE_WORD
    /* Read audio for both wake word detection and recording */
    pcm_len = hal_audio_read(g_pcm_buf, PCM_FRAME_SIZE);
    if (pcm_len < 0) {
        ESP_LOGE(TAG, "Audio read error");
        g_stats.error_count++;
        return -1;
    }
    if (pcm_len == 0) {
        return 0;  /* No data available */
    }

    int16_t *samples = (int16_t *)g_pcm_buf;
    size_t num_samples = pcm_len / 2;  /* 16-bit samples */

    /* Feed wake word detector when idle (local detection, no network) */
    if (g_state == VOICE_STATE_IDLE && g_wake_word_ctx != NULL) {
        hal_wake_word_feed(g_wake_word_ctx, samples, num_samples);
        /* Yield after feed so higher-priority detection task can call fetch()
         * before we loop back. Prevents AFE FEED ring buffer overflow. */
        taskYIELD();
    }

    /* Only send to WebSocket when recording */
    if (g_state != VOICE_STATE_RECORDING) {
        return 0;
    }
#else
    /* Original behavior: only read when recording */
    if (g_state != VOICE_STATE_RECORDING) {
        return 0;
    }

    pcm_len = hal_audio_read(g_pcm_buf, PCM_FRAME_SIZE);
    if (pcm_len < 0) {
        ESP_LOGE(TAG, "Audio read error");
        g_stats.error_count++;
        return -1;
    }
    if (pcm_len == 0) {
        ESP_LOGW(TAG, "Audio read: no data");
        return 0;  /* No data available */
    }

    int16_t *samples = (int16_t *)g_pcm_buf;
    size_t num_samples = pcm_len / 2;  /* 16-bit samples */
#endif

    /* Audio quality check: calculate RMS and peak */
    int sample_count = pcm_len / 2;
    int64_t sum_sq = 0;
    int16_t peak = 0;
    int zero_count = 0;

    for (int i = 0; i < sample_count; i++) {
        int16_t s = samples[i];
        if (s == 0) zero_count++;
        if (s < 0) s = -s;  /* abs */
        sum_sq += (int64_t)s * s;
        if (s > peak) peak = s;
    }

    int rms = (int)(sum_sq / sample_count);
    rms = (int)sqrt((double)rms);

    /* Log every 10 frames */
    if (g_stats.encode_count % 10 == 0) {
        ESP_LOGI(TAG, "Audio: frame#%d, rms=%d, peak=%d, zeros=%d/%d",
                 g_stats.encode_count + 1, rms, peak, zero_count, sample_count);
    }

#ifdef CONFIG_ENABLE_WAKE_WORD
    /* VAD: Check for silence timeout (only in wake word mode) */
    if (g_vad_enabled && vad_process_frame(rms)) {
        ESP_LOGI(TAG, "VAD triggered stop - silence timeout");
        /* Stop recording due to silence timeout */
        voice_recorder_process_event(VOICE_EVENT_TIMEOUT);
        display_update("Processing...", "thinking", 0, NULL);
        return 0;  /* Recording stopped, don't send this frame */
    }
#endif

    /* Send PCM directly via WebSocket (no encoding) */
    if (ws_send_audio(g_pcm_buf, pcm_len) != 0) {
        g_stats.error_count++;
        /* Only log every 10 errors to avoid flooding */
        if (g_stats.error_count % 10 == 1) {
            ESP_LOGE(TAG, "WS send audio failed (count: %d)", g_stats.error_count);
        }
        return -1;
    }

    g_stats.encode_count++;
    return 1;  /* One frame sent */
}

/* ------------------------------------------------------------------ */
/* Private: Button callback (called from task context via poll)        */
/* ------------------------------------------------------------------ */

static void button_callback(bool pressed)
{
    /* This is called from task context (via hal_button_poll) */
    if (pressed) {
        if (g_state == VOICE_STATE_IDLE) {
            /* Button triggers recording start */
            ESP_LOGI(TAG, "Button PRESSED - starting recording");
            g_recording_triggered_by_wake_word = false;
            voice_recorder_process_event(VOICE_EVENT_BUTTON_PRESS);
            display_update("Recording...", "listening", 0, NULL);
        } else if (g_state == VOICE_STATE_RECORDING) {
            /* Already recording (wake word mode) - short press to stop */
            ESP_LOGI(TAG, "Button PRESSED (short) - stopping recording (wake word mode)");
            voice_recorder_process_event(VOICE_EVENT_BUTTON_RELEASE);
            display_update("Processing...", "thinking", 0, NULL);
        }
    } else {
        /* Button RELEASED - only stop if triggered by button (long press mode) */
        if (g_state == VOICE_STATE_RECORDING && !g_recording_triggered_by_wake_word) {
            ESP_LOGI(TAG, "Button RELEASED - stopping recording");
            voice_recorder_process_event(VOICE_EVENT_BUTTON_RELEASE);
            display_update("Processing...", "thinking", 0, NULL);
        }
        /* If wake word triggered, ignore release (already stopped by short press) */
    }
}

/* ------------------------------------------------------------------ */
/* Private: Voice recorder task                                        */
/* ------------------------------------------------------------------ */

static TaskHandle_t g_voice_task_handle = NULL;
static volatile bool g_task_running = false;

/* Tick interval: 60ms for Opus frame size */
#define TICK_INTERVAL_MS    60

static void voice_recorder_task(void *arg)
{
    ESP_LOGI(TAG, "Voice recorder task started");

    while (g_task_running) {
        /* Poll button state via IO expander */
        hal_button_poll();

        /* Process audio encoding/sending if recording */
        voice_recorder_tick();

        vTaskDelay(pdMS_TO_TICKS(TICK_INTERVAL_MS));
    }

    g_voice_task_handle = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* Private: Wake word callback                                        */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_ENABLE_WAKE_WORD
static void on_wake_word_detected(const char *wake_word, void *user_data)
{
    ESP_LOGI(TAG, "Wake word detected: %s", wake_word);
    g_recording_triggered_by_wake_word = true;  /* Mark as wake word triggered */
    display_update("Listening...", "listening", 0, NULL);
    voice_recorder_process_event(VOICE_EVENT_WAKE_WORD);
}

static int wake_word_setup(void)
{
    wake_word_config_t config = {
        .model_path = NULL,  /* Use default */
        .callback = on_wake_word_detected,
        .user_data = NULL,
    };

#ifdef CONFIG_WAKE_WORD_CUSTOM
    config.wake_word_phrase = CONFIG_CUSTOM_WAKE_WORD_PHRASE;
    config.detection_threshold = (float)CONFIG_CUSTOM_WAKE_WORD_THRESHOLD / 100.0f;
#endif

    g_wake_word_ctx = hal_wake_word_init(&config);
    if (g_wake_word_ctx == NULL) {
        ESP_LOGE(TAG, "Failed to initialize wake word detector");
        return -1;
    }

    hal_wake_word_start(g_wake_word_ctx);
    ESP_LOGI(TAG, "Wake word detection enabled");
    return 0;
}

static void wake_word_cleanup(void)
{
    if (g_wake_word_ctx != NULL) {
        hal_wake_word_stop(g_wake_word_ctx);
        /* Wait for detection task to finish current fetch */
        vTaskDelay(pdMS_TO_TICKS(50));
        hal_wake_word_deinit(g_wake_word_ctx);
        g_wake_word_ctx = NULL;
    }
}
#endif /* CONFIG_ENABLE_WAKE_WORD */

/* ------------------------------------------------------------------ */
/* Public: Start voice recorder system (with button and task)         */
/* ------------------------------------------------------------------ */

int voice_recorder_start(void)
{
#ifdef CONFIG_ENABLE_WAKE_WORD
    /* Initialize wake word detector */
    if (wake_word_setup() != 0) {
        ESP_LOGW(TAG, "Wake word setup failed, continuing without wake word");
    }
    
    /* Start audio for wake word detection - must be running to feed AFE */
    ESP_LOGI(TAG, "Starting audio for wake word detection");
    if (hal_audio_start() != 0) {
        ESP_LOGE(TAG, "Audio start failed");
        return -1;
    }
#endif

    /* Initialize button via IO expander */
    if (hal_button_init(button_callback) != 0) {
        ESP_LOGE(TAG, "Button init failed");
        /* Continue anyway - voice recording may still work via other triggers */
    } else {
        ESP_LOGI(TAG, "Button initialized via IO expander");
    }

    /* Start voice recorder task */
    g_task_running = true;
    BaseType_t ret = xTaskCreate(
        voice_recorder_task,
        "voice_task",
        4096,
        NULL,
        5,
        &g_voice_task_handle
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Task create failed");
        g_task_running = false;
        return -1;
    }

    ESP_LOGI(TAG, "Voice recorder started");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public: Stop voice recorder system                                  */
/* ------------------------------------------------------------------ */

void voice_recorder_stop(void)
{
    g_task_running = false;

    if (g_voice_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(100));  /* Wait for task to exit */
    }

#ifdef CONFIG_ENABLE_WAKE_WORD
    /* Cleanup wake word detector */
    wake_word_cleanup();
#endif

    hal_button_deinit();
    ESP_LOGI(TAG, "Voice recorder stopped");
}

/* ------------------------------------------------------------------ */
/* Public: Pause wake word detection before TTS                        */
/* ------------------------------------------------------------------ */

void voice_recorder_pause_wake_word(void)
{
#ifdef CONFIG_ENABLE_WAKE_WORD
    if (g_wake_word_ctx != NULL) {
        ESP_LOGI(TAG, "Pausing wake word detection for TTS");
        hal_wake_word_stop(g_wake_word_ctx);
        /* Wait for detection task to finish current fetch */
        vTaskDelay(pdMS_TO_TICKS(50));
    }
#endif
}

/* ------------------------------------------------------------------ */
/* Public: Resume wake word detection after TTS                       */
/* ------------------------------------------------------------------ */

void voice_recorder_resume_wake_word(void)
{
#ifdef CONFIG_ENABLE_WAKE_WORD
    if (g_wake_word_ctx != NULL) {
        ESP_LOGI(TAG, "Resuming wake word detection");
        hal_wake_word_start(g_wake_word_ctx);
    }
#endif
}
