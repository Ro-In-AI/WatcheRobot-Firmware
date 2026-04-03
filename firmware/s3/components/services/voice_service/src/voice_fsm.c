#include "anim_player.h"
#include "behavior_state_service.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal_audio.h"
#include "hal_button.h"
#include "hal_wake_word.h"
#include "voice_service.h"
#include "ws_client.h"
#include <math.h>
#include <string.h>

#define TAG "VOICE"
#define LISTENING_UI_MIN_INTERNAL_FREE_BYTES (28U * 1024U)
#define LISTENING_UI_MIN_INTERNAL_LARGEST_BYTES (16U * 1024U)
#define LISTENING_UI_TEXT_ONLY_MIN_INTERNAL_FREE_BYTES (24U * 1024U)
#define LISTENING_UI_TEXT_ONLY_MIN_INTERNAL_LARGEST_BYTES (14U * 1024U)

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
#define PCM_FRAME_SIZE 1920

static uint8_t g_pcm_buf[PCM_FRAME_SIZE];

#if CONFIG_WATCHER_LOG_HEAP_DIAGNOSTICS
#define LOG_INTERNAL_HEAP_STATE(stage) log_internal_heap_state(stage)
static void log_internal_heap_state(const char *stage);
#else
#define LOG_INTERNAL_HEAP_STATE(stage) ((void)0)
#endif

static void show_cloud_not_ready_state(void) {
    bool connected = ws_client_is_connected() != 0;
    behavior_state_set_with_text(connected ? "processing" : "error",
                                 connected ? "Cloud Handshake..." : "Cloud Offline",
                                 0);
}

#if CONFIG_WATCHER_LOG_HEAP_DIAGNOSTICS
static void log_internal_heap_state(const char *stage) {
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "Internal heap @ %s: free=%u KB, largest=%u KB", stage, (unsigned)(free_internal / 1024U),
             (unsigned)(largest_internal / 1024U));
}
#endif

static bool has_listening_ui_headroom(size_t *free_internal_out, size_t *largest_internal_out) {
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (free_internal_out != NULL) {
        *free_internal_out = free_internal;
    }
    if (largest_internal_out != NULL) {
        *largest_internal_out = largest_internal;
    }

    return free_internal >= LISTENING_UI_MIN_INTERNAL_FREE_BYTES &&
           largest_internal >= LISTENING_UI_MIN_INTERNAL_LARGEST_BYTES;
}

static bool has_text_only_listening_ui_headroom(size_t *free_internal_out, size_t *largest_internal_out) {
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (free_internal_out != NULL) {
        *free_internal_out = free_internal;
    }
    if (largest_internal_out != NULL) {
        *largest_internal_out = largest_internal;
    }

    return free_internal >= LISTENING_UI_TEXT_ONLY_MIN_INTERNAL_FREE_BYTES &&
           largest_internal >= LISTENING_UI_TEXT_ONLY_MIN_INTERNAL_LARGEST_BYTES;
}

static bool can_freeze_animation_for_recording(size_t *free_internal_out, size_t *largest_internal_out) {
    return has_text_only_listening_ui_headroom(free_internal_out, largest_internal_out);
}

static void freeze_current_animation(void) {
    lvgl_port_lock(0);
    emoji_anim_stop();
    lvgl_port_unlock();
}

static void show_listening_ui(void) {
    size_t free_internal = 0;
    size_t largest_internal = 0;
    bool can_freeze = can_freeze_animation_for_recording(&free_internal, &largest_internal);

    if (can_freeze) {
        freeze_current_animation();
    } else {
        ESP_LOGW(TAG,
                 "Low internal heap, keeping current frame to avoid animation stop flush: free=%u KB largest=%u KB",
                 (unsigned)(free_internal / 1024U),
                 (unsigned)(largest_internal / 1024U));
    }

    if (has_listening_ui_headroom(&free_internal, &largest_internal)) {
        behavior_state_set_with_text("listening", "Listening...", 0);
        return;
    }

    if (has_text_only_listening_ui_headroom(&free_internal, &largest_internal)) {
        ESP_LOGW(TAG,
                 "Low internal heap, using text-only listening UI: free=%u KB largest=%u KB",
                 (unsigned)(free_internal / 1024U),
                 (unsigned)(largest_internal / 1024U));
        behavior_state_set_text_style("Listening...", 24, false);
        return;
    }

    ESP_LOGW(TAG,
             "Very low internal heap, skipping listening UI refresh: free=%u KB largest=%u KB",
             (unsigned)(free_internal / 1024U),
             (unsigned)(largest_internal / 1024U));
}

/* ------------------------------------------------------------------ */
/* Private: VAD (Voice Activity Detection)                             */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_ENABLE_WAKE_WORD
/* VAD state */
static int g_vad_silence_frames = 0; /* Consecutive silent frames */
static int g_vad_speech_frames = 0;  /* Total speech frames in this recording */

/* VAD configuration from Kconfig */
#define VAD_FRAME_MS 60 /* Each frame is 60ms */
#define VAD_SILENCE_FRAMES (CONFIG_VAD_SILENCE_TIMEOUT_MS / VAD_FRAME_MS)
#define VAD_RMS_THRESHOLD CONFIG_VAD_RMS_THRESHOLD
#define VAD_MIN_SPEECH_FRAMES (CONFIG_VAD_MIN_SPEECH_MS / VAD_FRAME_MS)

/* VAD control: only enable when wake word triggered */
static bool g_vad_enabled = false;

static void vad_reset(void) {
    g_vad_silence_frames = 0;
    g_vad_speech_frames = 0;
    g_vad_enabled = true;
    ESP_LOGI(TAG, "VAD reset, silence_threshold=%d frames, rms_threshold=%d, min_speech=%d frames", VAD_SILENCE_FRAMES,
             VAD_RMS_THRESHOLD, VAD_MIN_SPEECH_FRAMES);
}

static void vad_disable(void) {
    g_vad_enabled = false;
}

/**
 * Process VAD on a frame
 * @param rms RMS value of the audio frame
 * @return true if recording should stop (silence timeout)
 */
static bool vad_process_frame(int rms) {
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
            ESP_LOGI(TAG, "VAD: silence_frames=%d/%d, rms=%d (threshold=%d)", g_vad_silence_frames, VAD_SILENCE_FRAMES,
                     rms, VAD_RMS_THRESHOLD);
        }

        /* Check if silence timeout reached and minimum speech achieved */
        if (g_vad_silence_frames >= VAD_SILENCE_FRAMES && g_vad_speech_frames >= VAD_MIN_SPEECH_FRAMES) {
            ESP_LOGI(TAG, "VAD: Silence timeout detected! speech_frames=%d, silence_frames=%d", g_vad_speech_frames,
                     g_vad_silence_frames);
            return true; /* Signal to stop recording */
        }
    } else {
        /* Speech frame */
        g_vad_silence_frames = 0; /* Reset silence counter */
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

void voice_recorder_init(void) {
    g_state = VOICE_STATE_IDLE;
    memset(&g_stats, 0, sizeof(g_stats));
}

/* ------------------------------------------------------------------ */
/* Public: Get current state                                          */
/* ------------------------------------------------------------------ */

voice_state_t voice_recorder_get_state(void) {
    return g_state;
}

/* ------------------------------------------------------------------ */
/* Public: Reset statistics                                           */
/* ------------------------------------------------------------------ */

void voice_recorder_reset_stats(void) {
    g_stats.record_count = 0;
    g_stats.encode_count = 0;
    g_stats.error_count = 0;
}

/* ------------------------------------------------------------------ */
/* Public: Get statistics                                             */
/* ------------------------------------------------------------------ */

void voice_recorder_get_stats(voice_stats_t *out_stats) {
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

static int start_recording(void) {
    if (!ws_client_is_session_ready()) {
        ESP_LOGW(TAG, "start_recording blocked: ws session not ready (connected=%d)", ws_client_is_connected());
        show_cloud_not_ready_state();
        g_stats.error_count++;
        return -1;
    }

    LOG_INTERNAL_HEAP_STATE("before_recording");
    size_t free_internal = 0;
    size_t largest_internal = 0;
    if (can_freeze_animation_for_recording(&free_internal, &largest_internal)) {
        freeze_current_animation();
    } else {
        ESP_LOGW(TAG,
                 "Low internal heap, skipping animation freeze before recording: free=%u KB largest=%u KB",
                 (unsigned)(free_internal / 1024U),
                 (unsigned)(largest_internal / 1024U));
    }

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
    LOG_INTERNAL_HEAP_STATE("after_recording");
    ESP_LOGI(TAG, "start_recording: state -> RECORDING");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Private: Stop recording                                            */
/* ------------------------------------------------------------------ */

static int stop_recording(void) {
    /* In wake word mode, keep audio running for continuous detection */
#ifdef CONFIG_ENABLE_WAKE_WORD
    if (!g_recording_triggered_by_wake_word) {
        hal_audio_stop();
    }
    /* Wake word mode: audio stays running for next detection */
#else
    hal_audio_stop();
#endif

    /* Skip end marker if the cloud session is already gone. */
    if (ws_client_is_session_ready()) {
        if (ws_send_audio_end() != 0) {
            g_stats.error_count++;
            /* Still transition to idle */
        }
    } else {
        ESP_LOGW(TAG, "Skipping audio end marker: ws session not ready (connected=%d)", ws_client_is_connected());
    }

#ifdef CONFIG_ENABLE_WAKE_WORD
    /* Disable VAD when stopping */
    vad_disable();
#endif

    g_state = VOICE_STATE_IDLE;
    g_stats.record_count++;
    g_recording_triggered_by_wake_word = false; /* Reset trigger flag */
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public: Process event                                              */
/* ------------------------------------------------------------------ */

void voice_recorder_process_event(voice_event_t event) {
    switch (g_state) {
    case VOICE_STATE_IDLE:
        if (event == VOICE_EVENT_BUTTON_PRESS || event == VOICE_EVENT_WAKE_WORD) {
#ifdef CONFIG_ENABLE_WAKE_WORD
            if (event == VOICE_EVENT_WAKE_WORD) {
                ESP_LOGI(TAG, "Wake word triggered recording");
            }
#endif
            start_recording();
        }
        break;

    case VOICE_STATE_RECORDING:
        if (event == VOICE_EVENT_BUTTON_RELEASE || event == VOICE_EVENT_TIMEOUT) {
            stop_recording();
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Public: Process tick (read, send)                                   */
/* ------------------------------------------------------------------ */

int voice_recorder_tick(void) {
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
        return 0; /* No data available */
    }

    int16_t *samples = (int16_t *)g_pcm_buf;
    size_t num_samples = pcm_len / 2; /* 16-bit samples */

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
        return 0; /* No data available */
    }

    int16_t *samples = (int16_t *)g_pcm_buf;
#endif

    /* Audio quality check: calculate RMS and peak */
    int sample_count = pcm_len / 2;
    int64_t sum_sq = 0;
    int16_t peak = 0;
    int zero_count = 0;

    for (int i = 0; i < sample_count; i++) {
        int16_t s = samples[i];
        if (s == 0)
            zero_count++;
        if (s < 0)
            s = -s; /* abs */
        sum_sq += (int64_t)s * s;
        if (s > peak)
            peak = s;
    }

    int rms = (int)(sum_sq / sample_count);
    rms = (int)sqrt((double)rms);

    /* Log every 10 frames */
    if (g_stats.encode_count % 10 == 0) {
        ws_client_audio_queue_stats_t queue_stats = {0};
        ws_client_media_send_stats_t send_stats = {0};

        ws_client_get_audio_queue_stats(&queue_stats);
        ws_client_get_media_send_stats(&send_stats);
        ESP_LOGI(TAG,
                 "Audio: frame#%d rms=%d peak=%d zeros=%d/%d queue{pending=%u high=%u queued=%lu sent=%lu dropped=%lu delay=%lu end=%d first=%d} "
                 "send{total=%lu lock=%lu send=%lu payload=%u packet=%u}",
                 g_stats.encode_count + 1,
                 rms,
                 peak,
                 zero_count,
                 sample_count,
                 (unsigned int)queue_stats.pending_frames,
                 (unsigned int)queue_stats.high_watermark,
                 (unsigned long)queue_stats.queued_frames,
                 (unsigned long)queue_stats.sent_frames,
                 (unsigned long)queue_stats.dropped_frames,
                 (unsigned long)queue_stats.last_queue_delay_us,
                 queue_stats.end_pending,
                 queue_stats.first_frame_pending,
                 (unsigned long)send_stats.total_us,
                 (unsigned long)send_stats.lock_wait_us,
                 (unsigned long)send_stats.send_us,
                 (unsigned int)send_stats.payload_len,
                 (unsigned int)send_stats.packet_len);
    }

#ifdef CONFIG_ENABLE_WAKE_WORD
    /* VAD: Check for silence timeout (only in wake word mode) */
    if (g_vad_enabled && vad_process_frame(rms)) {
        ESP_LOGI(TAG, "VAD triggered stop - silence timeout");
        /* Stop recording due to silence timeout */
        voice_recorder_process_event(VOICE_EVENT_TIMEOUT);
        behavior_state_set_with_text("processing", "Processing...", 0);
        return 0; /* Recording stopped, don't send this frame */
    }
#endif

    /* Enqueue PCM for asynchronous WebSocket upload */
    if (ws_send_audio(g_pcm_buf, pcm_len) != 0) {
        if (!ws_client_is_session_ready()) {
            ESP_LOGW(TAG, "Cloud session lost during recording (connected=%d)", ws_client_is_connected());
            show_cloud_not_ready_state();
        }
        g_stats.error_count++;
        /* Only log every 10 errors to avoid flooding */
        if (g_stats.error_count % 10 == 1) {
            ESP_LOGE(TAG, "WS send audio failed (count: %d)", g_stats.error_count);
        }
        return -1;
    }

    g_stats.encode_count++;
    return 1; /* One frame sent */
}

/* ------------------------------------------------------------------ */
/* Private: Button callback (called from task context via poll)        */
/* ------------------------------------------------------------------ */

static void button_callback(bool pressed) {
    /* This is called from task context (via hal_button_poll) */
    if (pressed) {
        if (g_state == VOICE_STATE_IDLE) {
            if (!ws_client_is_session_ready()) {
                ESP_LOGW(TAG, "Button press ignored: ws session not ready (connected=%d)", ws_client_is_connected());
                show_cloud_not_ready_state();
                return;
            }
            /* Button triggers recording start */
            ESP_LOGI(TAG, "Button PRESSED - starting recording");
            g_recording_triggered_by_wake_word = false;
            voice_recorder_process_event(VOICE_EVENT_BUTTON_PRESS);
            if (g_state == VOICE_STATE_RECORDING) {
                show_listening_ui();
            } else {
                show_cloud_not_ready_state();
            }
        } else if (g_state == VOICE_STATE_RECORDING) {
            /* Already recording (wake word mode) - short press to stop */
            ESP_LOGI(TAG, "Button PRESSED (short) - stopping recording (wake word mode)");
            voice_recorder_process_event(VOICE_EVENT_BUTTON_RELEASE);
            behavior_state_set_with_text("processing", "Processing...", 0);
        }
    } else {
        /* Button RELEASED - only stop if triggered by button (long press mode) */
        if (g_state == VOICE_STATE_RECORDING && !g_recording_triggered_by_wake_word) {
            ESP_LOGI(TAG, "Button RELEASED - stopping recording");
            voice_recorder_process_event(VOICE_EVENT_BUTTON_RELEASE);
            behavior_state_set_with_text("processing", "Processing...", 0);
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
#define TICK_INTERVAL_MS 60
#define VOICE_TASK_EXIT_WAIT_MS 300

static bool voice_wait_for_task_exit(uint32_t timeout_ms) {
    uint32_t waited_ms = 0;

    while (g_voice_task_handle != NULL && waited_ms < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited_ms += 10;
    }

    return g_voice_task_handle == NULL;
}

static void voice_recorder_task(void *arg) {
    ESP_LOGI(TAG, "Voice recorder task started");

    while (g_task_running) {
        /* Poll button state via IO expander */
        hal_button_poll();

        /* Process audio capture/upload if recording */
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
static void on_wake_word_detected(const char *wake_word, void *user_data) {
    ESP_LOGI(TAG, "Wake word detected: %s", wake_word);
    g_recording_triggered_by_wake_word = true; /* Mark as wake word triggered */
    show_listening_ui();
    voice_recorder_process_event(VOICE_EVENT_WAKE_WORD);
}

static int wake_word_setup(void) {
    wake_word_config_t config = {
        .model_path = NULL, /* Use default */
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

static void wake_word_cleanup(void) {
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

int voice_recorder_start(void) {
    bool button_ready = hal_button_io_ready();

    if (g_task_running && g_voice_task_handle != NULL) {
        ESP_LOGI(TAG, "Voice recorder already running");
        return 0;
    }

    if (g_voice_task_handle != NULL && !voice_wait_for_task_exit(VOICE_TASK_EXIT_WAIT_MS)) {
        ESP_LOGW(TAG, "Voice recorder task is still stopping");
        return -1;
    }

#ifdef CONFIG_ENABLE_WAKE_WORD
    /* Avoid reserving audio DMA at boot. On S3 this can starve UI/WS/camera
     * of internal heap and lead to resets before the user even starts
     * recording. Keep the system in button-triggered mode and only request
     * audio when an actual recording begins. */
    ESP_LOGW(TAG, "Wake word boot activation disabled; audio will start on demand");
#endif

    if (!button_ready) {
        ESP_LOGW(TAG, "Voice button unavailable, starting recorder without button input");
    } else if (hal_button_init(button_callback) != 0) {
        ESP_LOGW(TAG, "Button init failed, continuing without button input");
    } else {
        ESP_LOGI(TAG, "Button initialized via IO expander");
    }

    /* Start voice recorder task */
    g_task_running = true;
    BaseType_t ret = xTaskCreate(voice_recorder_task, "voice_task", 4096, NULL, 5, &g_voice_task_handle);

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

void voice_recorder_stop(void) {
    bool had_runtime = g_task_running || g_voice_task_handle != NULL;

    g_task_running = false;

    if (g_voice_task_handle != NULL && !voice_wait_for_task_exit(VOICE_TASK_EXIT_WAIT_MS)) {
        ESP_LOGW(TAG, "Voice recorder task did not exit within %u ms", (unsigned)VOICE_TASK_EXIT_WAIT_MS);
    }

#ifdef CONFIG_ENABLE_WAKE_WORD
    /* Cleanup wake word detector */
    wake_word_cleanup();
#endif

    hal_button_deinit();
    if (had_runtime) {
        ESP_LOGI(TAG, "Voice recorder stopped");
    } else {
        ESP_LOGI(TAG, "Voice recorder already stopped");
    }
}

/* ------------------------------------------------------------------ */
/* Public: Pause wake word detection before TTS                        */
/* ------------------------------------------------------------------ */

void voice_recorder_pause_wake_word(void) {
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

void voice_recorder_resume_wake_word(void) {
#ifdef CONFIG_ENABLE_WAKE_WORD
    if (g_wake_word_ctx != NULL) {
        ESP_LOGI(TAG, "Resuming wake word detection");
        hal_wake_word_start(g_wake_word_ctx);
    }
#endif
}
