/**
 * @file ws_client.c
 * @brief WebSocket client implementation (Protocol v2.0)
 */

#include "ws_client.h"
#include "ws_router.h"
#include "display_ui.h"
#include "hal_audio.h"
#include "button_voice.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

#define TAG "WS_CLIENT"

/* WebSocket configuration */
#define WS_DEFAULT_URL  "ws://[IP_ADDRESS]"  /* Fallback if discovery fails */
#define WS_TIMEOUT_MS  10000
#define WS_URL_MAX_LEN 128
#define RESPONSE_TIMEOUT_MS  30000  /* 30 seconds timeout for server response */

static esp_websocket_client_handle_t ws_client = NULL;
static bool is_connected = false;
static bool tts_playing = false;  /* TTS playback state */
static bool waiting_for_response = false;
static int timeout_display_count = 0;  /* Limit timeout display to 1 time */
static int64_t response_wait_start_time = 0;  /* Timestamp when response wait started */
static char ws_server_url[WS_URL_MAX_LEN] = WS_DEFAULT_URL;  /* Dynamic server URL */

/* ------------------------------------------------------------------ */
/* WebSocket Event Handler                                            */
/* ------------------------------------------------------------------ */

static void ws_event_handler(void *handler_args, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket connected");
            is_connected = true;
            /* Show happy greeting when connected */
            display_update(NULL, "happy", 0, NULL);
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WebSocket disconnected");
            is_connected = false;
            /* Show standby when disconnected */
            display_update("Disconnected", "standby", 0, NULL);
            break;

        case WEBSOCKET_EVENT_DATA:
            if (data->op_code == WS_TRANSPORT_OPCODES_TEXT) {
                /* Handle server text messages */
                char *msg = strndup((char *)data->data_ptr, data->data_len);
                if (msg) {
                    /* Use ESP_LOGD to avoid flooding logs with high-frequency messages */
                    ESP_LOGI(TAG, "WS received: %s", msg);

                    /* End TTS playback when receiving tts_end or non-TTS message */
                    if (tts_playing) {
                        /* Check if this is tts_end message */
                        if (strstr(msg, "\"tts_end\"") != NULL) {
                            /* tts_end will be handled by router */
                        } else if (strstr(msg, "\"type\"") == NULL) {
                            /* Not a JSON message, end TTS */
                            ws_tts_complete();
                        }
                    }

                    /* Route JSON messages */
                    if (msg[0] == '{') {
                        ws_route_message(msg);
                    }

                    free(msg);
                }
            }
            else if (data->op_code == WS_TRANSPORT_OPCODES_BINARY) {
                /* Handle binary message (TTS audio - raw PCM) */
                ESP_LOGI(TAG, "WS received binary: %d bytes", data->data_len);
                ws_handle_tts_binary((const uint8_t *)data->data_ptr, data->data_len);
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error");
            break;

        default:
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Public: Initialize WebSocket Client                                */
/* ------------------------------------------------------------------ */

int ws_client_init(void)
{
    esp_websocket_client_config_t cfg = {
        .uri = ws_server_url,
        .network_timeout_ms = WS_TIMEOUT_MS,
        .buffer_size = 16384,  /* Increased for audio streaming (16KB) */
        .task_stack = 16384,   /* Increased stack size (16KB) */
    };

    ws_client = esp_websocket_client_init(&cfg);
    if (!ws_client) {
        ESP_LOGE(TAG, "Failed to init WebSocket client");
        return -1;
    }

    esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);

    ESP_LOGI(TAG, "WebSocket client initialized (URL: %s)", ws_server_url);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public: Set/Get Server URL                                         */
/* ------------------------------------------------------------------ */

int ws_client_set_server_url(const char *url)
{
    if (!url || strlen(url) >= WS_URL_MAX_LEN) {
        ESP_LOGE(TAG, "Invalid URL or URL too long");
        return -1;
    }

    /* Can only set URL before client is initialized */
    if (ws_client != NULL) {
        ESP_LOGW(TAG, "Cannot set URL after client initialized");
        return -1;
    }

    strncpy(ws_server_url, url, WS_URL_MAX_LEN - 1);
    ws_server_url[WS_URL_MAX_LEN - 1] = '\0';
    ESP_LOGI(TAG, "Server URL set to: %s", ws_server_url);
    return 0;
}

const char* ws_client_get_server_url(void)
{
    return ws_server_url;
}

/* ------------------------------------------------------------------ */
/* Public: Start/Stop Connection                                      */
/* ------------------------------------------------------------------ */

int ws_client_start(void)
{
    if (!ws_client) {
        ESP_LOGE(TAG, "WebSocket not initialized");
        return -1;
    }

    esp_err_t ret = esp_websocket_client_start(ws_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket: %s", esp_err_to_name(ret));
        return -1;
    }

    return 0;
}

void ws_client_stop(void)
{
    if (ws_client) {
        esp_websocket_client_stop(ws_client);
        esp_websocket_client_destroy(ws_client);
        ws_client = NULL;
        is_connected = false;
    }
}

/* ------------------------------------------------------------------ */
/* Public: Send Functions                                             */
/* ------------------------------------------------------------------ */

int ws_client_send_binary(const uint8_t *data, int len)
{
    if (!ws_client || !is_connected) {
        return -1;
    }

    int sent = esp_websocket_client_send_bin(ws_client, (const char *)data, len, pdMS_TO_TICKS(1000));
    return sent;
}

int ws_client_send_text(const char *text)
{
    if (!ws_client || !is_connected) {
        return -1;
    }

    int sent = esp_websocket_client_send_text(ws_client, text, strlen(text), pdMS_TO_TICKS(1000));
    return sent;
}

int ws_client_is_connected(void)
{
    return is_connected ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Implementation of WebSocket interface for button_voice             */
/* ------------------------------------------------------------------ */

/**
 * Audio protocol (v2.0):
 *
 * Raw PCM binary frame (no header):
 *   [0-n]   PCM audio data (16-bit, 16kHz, mono)
 *
 * Simpler and more efficient for MVP.
 */

int ws_send_audio(const uint8_t *data, int len)
{
    if (!ws_client || !is_connected || len <= 0) {
        ESP_LOGW(TAG, "ws_send_audio: not ready (conn=%d, len=%d)", is_connected, len);
        return -1;
    }

    /* Send raw PCM directly, no header */
    /* Increased timeout to 5 seconds for better reliability */
    int sent = esp_websocket_client_send_bin(ws_client, (const char *)data, len, pdMS_TO_TICKS(5000));

    if (sent != len) {
        ESP_LOGW(TAG, "Audio send incomplete: %d/%d", sent, len);
        return -1;
    }

    return 0;
}

int ws_send_audio_end(void)
{
    /* Start response timeout timer */
    waiting_for_response = true;
    timeout_display_count = 0;  /* Reset timeout display counter */
    response_wait_start_time = esp_timer_get_time();
    ESP_LOGI(TAG, "Audio end sent, waiting for response (timeout %dms)", RESPONSE_TIMEOUT_MS);

    /* Send audio end marker (v2.0 protocol: "over") */
    return ws_client_send_text("over");
}

/* ------------------------------------------------------------------ */
/* TTS Binary Frame Handling (v2.0 - Raw PCM)                         */
/* ------------------------------------------------------------------ */

/**
 * Play TTS audio from binary frame (Raw PCM)
 *
 * Frame format (v2.0):
 *   [0-n]   PCM audio data (16-bit, 24kHz, mono)
 *
 * @param data Binary frame data
 * @param len Frame length
 */
void ws_handle_tts_binary(const uint8_t *data, int len)
{
    if (!data || len <= 0) {
        ESP_LOGW(TAG, "TTS frame empty: %d bytes", len);
        return;
    }

    /* Only update display and start audio on first chunk */
    if (!tts_playing) {
        ESP_LOGI(TAG, "TTS started, first chunk: %d bytes", len);
        /* Clear response wait flag - server has responded with TTS */
        waiting_for_response = false;
        display_update("", "speaking", 0, NULL);

#ifdef CONFIG_ENABLE_WAKE_WORD
        /* Pause wake word detection before TTS to avoid I2S conflicts */
        voice_recorder_pause_wake_word();
#endif

        /* Mark as playback mode to skip unnecessary I2S stop in sample rate switch */
        hal_audio_set_playback_mode(true);

        /* Switch to 24kHz for TTS playback (火山引擎 TTS) */
        hal_audio_set_sample_rate(24000);
        hal_audio_start();
        tts_playing = true;
    }

    /* Play raw PCM directly (no AUD1 header in v2.0) */
    ESP_LOGD(TAG, "Playing PCM: %d bytes", len);
    int written = hal_audio_write(data, len);
    if (written != len) {
        ESP_LOGW(TAG, "TTS playback incomplete: %d/%d", written, len);
    }
}

/**
 * Signal TTS playback complete (called by application or tts_end handler)
 * Waits for I2S DMA buffer to finish playing before switching state
 */
void ws_tts_complete(void)
{
    /* Clear response wait flag regardless of tts_playing state */
    waiting_for_response = false;

    if (tts_playing) {
        ESP_LOGI(TAG, "TTS playback complete");

        /* Wait for I2S DMA buffer to finish playing (~500ms buffer) */
        vTaskDelay(pdMS_TO_TICKS(500));

        hal_audio_stop();
        /* Restore recording mode before sample rate switch (avoid I2S error) */
        hal_audio_set_playback_mode(false);

        /* Wait 1 second for DMA buffer to fully drain before switching sample rate */
        vTaskDelay(pdMS_TO_TICKS(1000));
        /* Restore 16kHz for recording */
        hal_audio_set_sample_rate(16000);

        display_update(NULL, "happy", 0, NULL);
        tts_playing = false;

        /* Restore recording mode for wake word detection */
    }

#ifdef CONFIG_ENABLE_WAKE_WORD
    /* Always resume wake word detection after tts_end, regardless of TTS playback state */
    ESP_LOGI(TAG, "TTS complete, resuming wake word detection");
    hal_audio_start();
    voice_recorder_resume_wake_word();
#endif
}

/**
 * @brief Check TTS timeout and auto-complete if needed
 *
 * Call this periodically from main loop. If no TTS data received
 * for a while, auto-complete the playback.
 * Note: In v2.0 protocol, server should send tts_end message.
 * This is kept as a fallback.
 */
void ws_tts_timeout_check(void)
{
#ifdef CONFIG_ENABLE_WAKE_WORD
    /* Check if we've been waiting too long for a response */
    if (waiting_for_response) {
        int64_t elapsed_ms = (esp_timer_get_time() - response_wait_start_time) / 1000;
        if (elapsed_ms > RESPONSE_TIMEOUT_MS && timeout_display_count < 1) {
            ESP_LOGW(TAG, "Response timeout (%lld ms), resuming wake word detection", elapsed_ms);
            waiting_for_response = false;

            /* Resume wake word detection */
            voice_recorder_resume_wake_word();
            display_update("Timeout", "sad", 0, NULL);
            timeout_display_count++;
        }
    }
#endif
}
