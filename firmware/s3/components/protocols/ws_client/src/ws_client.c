/**
 * @file ws_client.c
 * @brief WebSocket client implementation (Protocol v2.0)
 */

#include "ws_client.h"
#include "camera_service.h"
#include "display_ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal_audio.h"
#include "ws_router.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "WS_CLIENT"

/* WebSocket configuration */
#define WS_DEFAULT_URL "ws://[IP_ADDRESS]" /* Fallback if discovery fails */
#define WS_TIMEOUT_MS 10000
#define WS_URL_MAX_LEN 128
#define RESPONSE_TIMEOUT_MS 30000 /* 30 seconds timeout for server response */
#define WS_BUFFER_SIZE 65536
#define WS_TASK_STACK 24576
#define WS_TEXT_PAYLOAD_MAX 384
#define WS_BINARY_HEADER_LEN 16
#define WS_BINARY_MAGIC "WSPK"
#define WS_BINARY_FRAME_VIDEO 2
#define WS_BINARY_FRAME_IMAGE 3
#define WS_BINARY_FLAG_FIRST 0x01
#define WS_BINARY_FLAG_LAST 0x02
#define WS_BINARY_FLAG_KEYFRAME 0x04

static esp_websocket_client_handle_t ws_client = NULL;
static bool is_connected = false;
static bool tts_playing = false; /* TTS playback state */
static bool waiting_for_response = false;
static int timeout_display_count = 0;                       /* Limit timeout display to 1 time */
static int64_t response_wait_start_time = 0;                /* Timestamp when response wait started */
static char ws_server_url[WS_URL_MAX_LEN] = WS_DEFAULT_URL; /* Dynamic server URL */
static SemaphoreHandle_t ws_send_lock = NULL;

static int ws_client_lock_and_send(bool binary, const void *payload, int len) {
    int sent = -1;

    if (!ws_client || !is_connected || payload == NULL || len < 0) {
        return -1;
    }

    if (ws_send_lock == NULL) {
        ws_send_lock = xSemaphoreCreateMutex();
        if (ws_send_lock == NULL) {
            ESP_LOGE(TAG, "create ws send lock failed");
            return -1;
        }
    }

    if (xSemaphoreTake(ws_send_lock, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGW(TAG, "ws send lock timeout");
        return -1;
    }

    sent = binary ? esp_websocket_client_send_bin(ws_client, (const char *)payload, len, pdMS_TO_TICKS(1000))
                  : esp_websocket_client_send_text(ws_client, (const char *)payload, len, pdMS_TO_TICKS(1000));

    xSemaphoreGive(ws_send_lock);
    return sent;
}

/* ------------------------------------------------------------------ */
/* WebSocket Event Handler                                            */
/* ------------------------------------------------------------------ */

static void ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
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
        if (camera_service_is_streaming()) {
            ESP_LOGW(TAG, "stopping camera stream on WebSocket disconnect");
            camera_service_stop_stream();
        }
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
        } else if (data->op_code == WS_TRANSPORT_OPCODES_BINARY) {
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

int ws_client_init(void) {
    esp_websocket_client_config_t cfg = {
        .uri = ws_server_url,
        .network_timeout_ms = WS_TIMEOUT_MS,
        .buffer_size = WS_BUFFER_SIZE,
        .task_stack = WS_TASK_STACK,
    };

    ws_client = esp_websocket_client_init(&cfg);
    if (!ws_client) {
        ESP_LOGE(TAG, "Failed to init WebSocket client");
        return -1;
    }

    if (ws_send_lock == NULL) {
        ws_send_lock = xSemaphoreCreateMutex();
        if (ws_send_lock == NULL) {
            esp_websocket_client_destroy(ws_client);
            ws_client = NULL;
            ESP_LOGE(TAG, "Failed to create WebSocket send lock");
            return -1;
        }
    }

    esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);

    ESP_LOGI(TAG, "WebSocket client initialized (URL: %s)", ws_server_url);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public: Set/Get Server URL                                         */
/* ------------------------------------------------------------------ */

int ws_client_set_server_url(const char *url) {
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

const char *ws_client_get_server_url(void) {
    return ws_server_url;
}

/* ------------------------------------------------------------------ */
/* Public: Start/Stop Connection                                      */
/* ------------------------------------------------------------------ */

int ws_client_start(void) {
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

void ws_client_stop(void) {
    if (camera_service_is_streaming()) {
        camera_service_stop_stream();
    }

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

int ws_client_send_binary(const uint8_t *data, int len) {
    if (!ws_client || !is_connected || data == NULL || len < 0) {
        return -1;
    }

    return ws_client_lock_and_send(true, data, len);
}

int ws_client_send_text(const char *text) {
    if (!ws_client || !is_connected || text == NULL) {
        return -1;
    }

    return ws_client_lock_and_send(false, text, (int)strlen(text));
}

int ws_client_is_connected(void) {
    return is_connected ? 1 : 0;
}

static void ws_write_u16_le(uint8_t *dst, uint16_t value) {
    dst[0] = (uint8_t)(value & 0xFF);
    dst[1] = (uint8_t)((value >> 8) & 0xFF);
}

static void ws_write_u32_le(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)(value & 0xFF);
    dst[1] = (uint8_t)((value >> 8) & 0xFF);
    dst[2] = (uint8_t)((value >> 16) & 0xFF);
    dst[3] = (uint8_t)((value >> 24) & 0xFF);
}

static int ws_send_binary_packet(uint8_t frame_type, uint8_t flags, uint16_t stream_id, uint32_t seq,
                                 const uint8_t *payload, size_t len) {
    uint8_t *packet = NULL;
    size_t packet_len = WS_BINARY_HEADER_LEN + len;
    int sent;

    if (!ws_client || !is_connected) {
        return -1;
    }

    if (len > 0 && payload == NULL) {
        return -1;
    }

    packet = (uint8_t *)malloc(packet_len);
    if (!packet) {
        ESP_LOGE(TAG, "binary packet alloc failed, len=%u", (unsigned int)packet_len);
        return -1;
    }

    memcpy(packet, WS_BINARY_MAGIC, 4);
    packet[4] = frame_type;
    packet[5] = flags;
    ws_write_u16_le(packet + 6, stream_id);
    ws_write_u32_le(packet + 8, seq);
    ws_write_u32_le(packet + 12, (uint32_t)len);
    if (len > 0) {
        memcpy(packet + WS_BINARY_HEADER_LEN, payload, len);
    }

    sent = ws_client_send_binary(packet, (int)packet_len);
    free(packet);

    if (sent != (int)packet_len) {
        ESP_LOGW(TAG, "binary packet send incomplete: %d/%u", sent, (unsigned int)packet_len);
        return -1;
    }

    return 0;
}

int ws_send_sys_ack(const char *command_id, const char *command_type, uint16_t stream_id, const char *message) {
    char payload[WS_TEXT_PAYLOAD_MAX];
    int len;

    if (!command_id || command_id[0] == '\0' || !command_type || !ws_client || !is_connected) {
        return -1;
    }

    if (message && message[0] != '\0') {
        len = snprintf(payload, sizeof(payload),
                       "{\"type\":\"sys.ack\",\"code\":0,\"data\":{\"command_id\":\"%s\",\"command_type\":\"%s\","
                       "\"stream_id\":%u,\"message\":\"%s\"}}",
                       command_id,
                       command_type,
                       (unsigned int)stream_id,
                       message);
    } else {
        len = snprintf(payload, sizeof(payload),
                       "{\"type\":\"sys.ack\",\"code\":0,\"data\":{\"command_id\":\"%s\",\"command_type\":\"%s\","
                       "\"stream_id\":%u}}",
                       command_id,
                       command_type,
                       (unsigned int)stream_id);
    }

    if (len <= 0 || len >= (int)sizeof(payload)) {
        ESP_LOGE(TAG, "ack payload overflow");
        return -1;
    }

    return ws_client_send_text(payload);
}

int ws_send_sys_nack(const char *command_id, const char *command_type, const char *reason) {
    char payload[WS_TEXT_PAYLOAD_MAX];
    int len;

    if (!command_type || !reason || !ws_client || !is_connected) {
        return -1;
    }

    if (command_id && command_id[0] != '\0') {
        len = snprintf(payload, sizeof(payload),
                       "{\"type\":\"sys.nack\",\"code\":1,\"data\":{\"command_id\":\"%s\",\"command_type\":\"%s\","
                       "\"reason\":\"%s\"}}",
                       command_id,
                       command_type,
                       reason);
    } else {
        len = snprintf(payload, sizeof(payload),
                       "{\"type\":\"sys.nack\",\"code\":1,\"data\":{\"command_type\":\"%s\",\"reason\":\"%s\"}}",
                       command_type,
                       reason);
    }

    if (len <= 0 || len >= (int)sizeof(payload)) {
        ESP_LOGE(TAG, "nack payload overflow");
        return -1;
    }

    return ws_client_send_text(payload);
}

int ws_send_camera_state(const char *action, const char *state, uint16_t stream_id, int fps, const char *message) {
    char payload[WS_TEXT_PAYLOAD_MAX];
    int len;

    if (!action || !state || !ws_client || !is_connected) {
        return -1;
    }

    if (message && message[0] != '\0') {
        len = snprintf(payload, sizeof(payload),
                       "{\"type\":\"evt.camera.state\",\"code\":0,\"data\":{\"action\":\"%s\",\"state\":\"%s\","
                       "\"stream_id\":%u,\"fps\":%d,\"message\":\"%s\"}}",
                       action,
                       state,
                       (unsigned int)stream_id,
                       fps,
                       message);
    } else {
        len = snprintf(payload, sizeof(payload),
                       "{\"type\":\"evt.camera.state\",\"code\":0,\"data\":{\"action\":\"%s\",\"state\":\"%s\","
                       "\"stream_id\":%u,\"fps\":%d}}",
                       action,
                       state,
                       (unsigned int)stream_id,
                       fps);
    }

    if (len <= 0 || len >= (int)sizeof(payload)) {
        ESP_LOGE(TAG, "camera state payload overflow");
        return -1;
    }

    return ws_client_send_text(payload);
}

int ws_send_video_frame(const uint8_t *jpeg, size_t len, uint16_t stream_id, uint32_t seq, bool first_frame) {
    uint8_t flags = WS_BINARY_FLAG_KEYFRAME;

    if (!jpeg || len == 0) {
        return -1;
    }

    if (first_frame) {
        flags |= WS_BINARY_FLAG_FIRST;
    }

    return ws_send_binary_packet(WS_BINARY_FRAME_VIDEO, flags, stream_id, seq, jpeg, len);
}

int ws_send_video_end(uint16_t stream_id, uint32_t seq) {
    return ws_send_binary_packet(WS_BINARY_FRAME_VIDEO, WS_BINARY_FLAG_LAST, stream_id, seq, NULL, 0);
}

int ws_send_image_frame(const uint8_t *jpeg, size_t len, uint16_t stream_id) {
    if (!jpeg || len == 0) {
        return -1;
    }

    return ws_send_binary_packet(WS_BINARY_FRAME_IMAGE,
                                 WS_BINARY_FLAG_FIRST | WS_BINARY_FLAG_LAST | WS_BINARY_FLAG_KEYFRAME,
                                 stream_id,
                                 1,
                                 jpeg,
                                 len);
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

int ws_send_audio(const uint8_t *data, int len) {
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

int ws_send_audio_end(void) {
    /* Start response timeout timer */
    waiting_for_response = true;
    timeout_display_count = 0; /* Reset timeout display counter */
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
void ws_handle_tts_binary(const uint8_t *data, int len) {
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
void ws_tts_complete(void) {
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
void ws_tts_timeout_check(void) {
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
