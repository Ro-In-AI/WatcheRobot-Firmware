/**
 * @file ws_client.c
 * @brief WebSocket client implementation (Watcher protocol v0.1.1)
 */

#include "ws_client.h"

#include "camera_service.h"
#include "cJSON.h"
#include "display_ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal_audio.h"
#include "ota_service.h"
#include "voice_service.h"
#include "ws_router.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "WS_CLIENT"

#define WS_DEFAULT_URL "ws://[IP_ADDRESS]"
#define WS_TIMEOUT_MS 10000
#define WS_URL_MAX_LEN 128
#define WS_BUFFER_SIZE 65536
#define WS_TASK_STACK 16384
#define WS_RESPONSE_TIMEOUT_MS 30000
#define WS_BINARY_HEADER_LEN 14
#define WS_BINARY_MAGIC "WSPK"
#define WS_DEVICE_ERROR_CODE_GENERIC 1501

static esp_websocket_client_handle_t s_ws_client = NULL;
static bool s_socket_connected = false;
static bool s_hello_acknowledged = false;
static bool s_tts_playing = false;
static bool s_waiting_for_response = false;
static bool s_audio_upload_active = false;
static bool s_ota_binary_nacked = false;
static int s_timeout_display_count = 0;
static int64_t s_response_wait_start_time = 0;
static char s_ws_server_url[WS_URL_MAX_LEN] = WS_DEFAULT_URL;
static SemaphoreHandle_t s_ws_send_lock = NULL;
static uint32_t s_frame_sequences[WS_FRAME_TYPE_OTA + 1] = {0};
static ws_client_media_send_stats_t s_last_media_send_stats = {0};

static void ws_write_u32_le(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)(value & 0xFF);
    dst[1] = (uint8_t)((value >> 8) & 0xFF);
    dst[2] = (uint8_t)((value >> 16) & 0xFF);
    dst[3] = (uint8_t)((value >> 24) & 0xFF);
}

static uint32_t ws_read_u32_le(const uint8_t *src) {
    return ((uint32_t)src[0]) | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

static void ws_reset_media_state(void) {
    memset(s_frame_sequences, 0, sizeof(s_frame_sequences));
    memset(&s_last_media_send_stats, 0, sizeof(s_last_media_send_stats));
    s_audio_upload_active = false;
    s_ota_binary_nacked = false;
}

static void ws_reset_session_state(void) {
    s_socket_connected = false;
    s_hello_acknowledged = false;
    s_waiting_for_response = false;
    s_timeout_display_count = 0;
    s_response_wait_start_time = 0;
    ws_reset_media_state();
}

static void ws_resume_wake_word_after_tts(void) {
#ifdef CONFIG_ENABLE_WAKE_WORD
    hal_audio_start();
    voice_recorder_resume_wake_word();
#endif
}

static void ws_abort_tts_playback(void) {
    s_waiting_for_response = false;

    if (!s_tts_playing) {
        ws_resume_wake_word_after_tts();
        return;
    }

    hal_audio_stop();
    hal_audio_set_playback_mode(false);
    hal_audio_set_sample_rate(16000);
    s_tts_playing = false;
    ws_resume_wake_word_after_tts();
}

static int ws_client_lock_and_send(bool binary, const void *payload, int len, bool allow_before_session) {
    int sent = -1;
    int64_t start_us = 0;
    int64_t lock_acquired_us = 0;
    int64_t send_done_us = 0;

    if (s_ws_client == NULL || payload == NULL || len < 0 || !s_socket_connected) {
        return -1;
    }

    if (!allow_before_session && !s_hello_acknowledged) {
        return -1;
    }

    if (s_ws_send_lock == NULL) {
        s_ws_send_lock = xSemaphoreCreateMutex();
        if (s_ws_send_lock == NULL) {
            ESP_LOGE(TAG, "create ws send lock failed");
            return -1;
        }
    }

    start_us = esp_timer_get_time();
    if (xSemaphoreTake(s_ws_send_lock, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGW(TAG, "ws send lock timeout");
        return -1;
    }
    lock_acquired_us = esp_timer_get_time();

    sent = binary ? esp_websocket_client_send_bin(s_ws_client, (const char *)payload, len, pdMS_TO_TICKS(1000))
                  : esp_websocket_client_send_text(s_ws_client, (const char *)payload, len, pdMS_TO_TICKS(1000));
    send_done_us = esp_timer_get_time();

    xSemaphoreGive(s_ws_send_lock);

    s_last_media_send_stats.valid = false;
    s_last_media_send_stats.binary = binary;
    s_last_media_send_stats.lock_wait_us = (uint32_t)(lock_acquired_us - start_us);
    s_last_media_send_stats.send_us = (uint32_t)(send_done_us - lock_acquired_us);
    s_last_media_send_stats.total_us = (uint32_t)(send_done_us - start_us);
    s_last_media_send_stats.timestamp_us = (uint64_t)send_done_us;

    return sent;
}

static int ws_send_text_internal(const char *text, bool allow_before_session) {
    if (text == NULL) {
        return -1;
    }

    return ws_client_lock_and_send(false, text, (int)strlen(text), allow_before_session);
}

static int ws_send_json_root(cJSON *root, bool allow_before_session) {
    char *json = NULL;
    int sent = -1;

    if (root == NULL) {
        return -1;
    }

    json = cJSON_PrintUnformatted(root);
    if (json == NULL) {
        ESP_LOGE(TAG, "json encode failed");
        return -1;
    }

    sent = ws_send_text_internal(json, allow_before_session);
    cJSON_free(json);
    return sent;
}

static int ws_send_json_envelope(const char *type, int code, cJSON *data, bool allow_before_session) {
    cJSON *root = NULL;
    int sent = -1;

    if (type == NULL) {
        return -1;
    }

    root = cJSON_CreateObject();
    if (root == NULL) {
        cJSON_Delete(data);
        return -1;
    }

    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddNumberToObject(root, "code", code);
    if (data != NULL) {
        cJSON_AddItemToObject(root, "data", data);
    } else {
        cJSON_AddNullToObject(root, "data");
    }

    sent = ws_send_json_root(root, allow_before_session);
    cJSON_Delete(root);
    return sent;
}

static uint32_t ws_next_frame_seq(ws_frame_type_t frame_type) {
    if (frame_type <= 0 || frame_type > WS_FRAME_TYPE_OTA) {
        return 0;
    }

    s_frame_sequences[frame_type] += 1U;
    if (s_frame_sequences[frame_type] == 0U) {
        s_frame_sequences[frame_type] = 1U;
    }

    return s_frame_sequences[frame_type];
}

static int ws_send_binary_packet(ws_frame_type_t frame_type, uint8_t flags, const uint8_t *payload, size_t len) {
    uint8_t *packet = NULL;
    size_t packet_len = WS_BINARY_HEADER_LEN + len;
    uint32_t seq;
    int sent;
    bool ok;

    if (frame_type <= 0 || frame_type > WS_FRAME_TYPE_OTA) {
        return -1;
    }
    if (len > 0 && payload == NULL) {
        return -1;
    }

    seq = ws_next_frame_seq(frame_type);
    if (seq == 0U) {
        return -1;
    }

    packet = (uint8_t *)malloc(packet_len);
    if (packet == NULL) {
        ESP_LOGE(TAG, "binary packet alloc failed, len=%u", (unsigned int)packet_len);
        return -1;
    }

    memcpy(packet, WS_BINARY_MAGIC, 4);
    packet[4] = (uint8_t)frame_type;
    packet[5] = flags;
    ws_write_u32_le(packet + 6, seq);
    ws_write_u32_le(packet + 10, (uint32_t)len);
    if (len > 0) {
        memcpy(packet + WS_BINARY_HEADER_LEN, payload, len);
    }

    sent = ws_client_lock_and_send(true, packet, (int)packet_len, false);
    free(packet);
    ok = (sent == (int)packet_len);

    s_last_media_send_stats.valid = ok;
    s_last_media_send_stats.frame_type = (uint8_t)frame_type;
    s_last_media_send_stats.payload_len = len;
    s_last_media_send_stats.packet_len = (uint32_t)packet_len;

    if (!ok) {
        ESP_LOGW(TAG, "binary packet send incomplete: %d/%u", sent, (unsigned int)packet_len);
        return -1;
    }

    return 0;
}

static void ws_get_mac_string(char *out, size_t out_size) {
    uint8_t mac[6] = {0};

    if (out == NULL || out_size == 0U) {
        return;
    }

    out[0] = '\0';
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) {
        return;
    }

    snprintf(out,
             out_size,
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0],
             mac[1],
             mac[2],
             mac[3],
             mac[4],
             mac[5]);
}

static int ws_send_client_hello(void) {
    cJSON *data = cJSON_CreateObject();

    if (data == NULL) {
        return -1;
    }

    cJSON_AddStringToObject(data, "role", "hardware");
    cJSON_AddStringToObject(data, "fw_version", ota_service_get_fw_version());
    return ws_send_json_envelope("sys.client.hello", 0, data, true);
}

static bool ws_event_is_fragmented(const esp_websocket_event_data_t *data) {
    return (data != NULL) && (data->payload_offset != 0 || data->payload_len != data->data_len);
}

static void ws_handle_text_frame(const esp_websocket_event_data_t *data) {
    char *msg = NULL;
    ws_msg_type_t msg_type;

    if (data == NULL || data->data_ptr == NULL || data->data_len <= 0) {
        return;
    }

    if (ws_event_is_fragmented(data)) {
        ESP_LOGW(TAG,
                 "fragmented text frame unsupported: offset=%d chunk=%d total=%d",
                 data->payload_offset,
                 data->data_len,
                 data->payload_len);
        return;
    }

    msg = (char *)malloc((size_t)data->data_len + 1U);
    if (msg == NULL) {
        return;
    }

    memcpy(msg, data->data_ptr, (size_t)data->data_len);
    msg[data->data_len] = '\0';

    ESP_LOGI(TAG, "WS received: %s", msg);
    msg_type = ws_route_message(msg);
    switch (msg_type) {
    case WS_MSG_SYS_ACK:
    case WS_MSG_SYS_NACK:
    case WS_MSG_EVT_ASR_RESULT:
    case WS_MSG_EVT_AI_STATUS:
    case WS_MSG_EVT_AI_THINKING:
    case WS_MSG_EVT_AI_REPLY:
        ws_client_mark_server_response();
        break;
    default:
        break;
    }

    free(msg);
}

static void ws_handle_binary_frame(const esp_websocket_event_data_t *data) {
    const uint8_t *frame = NULL;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    uint8_t frame_type;
    uint8_t flags;

    if (data == NULL || data->data_ptr == NULL || data->data_len < WS_BINARY_HEADER_LEN) {
        return;
    }

    if (ws_event_is_fragmented(data)) {
        ESP_LOGW(TAG,
                 "fragmented binary frame unsupported: offset=%d chunk=%d total=%d",
                 data->payload_offset,
                 data->data_len,
                 data->payload_len);
        return;
    }

    frame = (const uint8_t *)data->data_ptr;
    if (memcmp(frame, WS_BINARY_MAGIC, 4) != 0) {
        ESP_LOGW(TAG, "invalid binary magic");
        return;
    }

    frame_type = frame[4];
    flags = frame[5];
    payload_len = (size_t)ws_read_u32_le(frame + 10);
    if (payload_len > (size_t)(data->data_len - WS_BINARY_HEADER_LEN)) {
        ESP_LOGW(TAG, "invalid binary payload len=%u packet=%d", (unsigned int)payload_len, data->data_len);
        return;
    }

    payload = frame + WS_BINARY_HEADER_LEN;
    switch (frame_type) {
    case WS_FRAME_TYPE_AUDIO:
        if (payload_len > 0U) {
            ws_handle_tts_binary(payload, (int)payload_len);
        }
        ws_client_mark_server_response();
        if ((flags & WS_FRAME_FLAG_LAST) != 0U) {
            ws_tts_complete();
        }
        break;
    case WS_FRAME_TYPE_OTA:
        ESP_LOGW(TAG, "OTA binary frame not supported");
        if (!s_ota_binary_nacked) {
            s_ota_binary_nacked = true;
            ws_send_ota_progress(0, "rejected", "not_supported");
            ws_send_device_error(WS_DEVICE_ERROR_CODE_GENERIC, "ota_binary_not_supported");
            ws_send_sys_nack("xfer.ota.handshake", NULL, "not_supported");
        }
        break;
    default:
        ESP_LOGW(TAG, "unexpected binary frame type=%u len=%u", frame_type, (unsigned int)payload_len);
        break;
    }
}

static void ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    (void)handler_args;
    (void)base;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        s_socket_connected = true;
        s_hello_acknowledged = false;
        s_waiting_for_response = false;
        s_timeout_display_count = 0;
        s_response_wait_start_time = 0;
        ws_reset_media_state();
        if (ws_send_client_hello() < 0) {
            ESP_LOGE(TAG, "failed to send sys.client.hello");
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected");
        if (camera_service_is_streaming()) {
            ESP_LOGW(TAG, "stopping camera stream on WebSocket disconnect");
            camera_service_stop_stream();
        }
        ws_abort_tts_playback();
        ws_reset_session_state();
        display_update("Disconnected", "standby", 0, NULL);
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data == NULL) {
            break;
        }
        if (data->op_code == WS_TRANSPORT_OPCODES_TEXT) {
            ws_handle_text_frame(data);
        } else if (data->op_code == WS_TRANSPORT_OPCODES_BINARY) {
            ws_handle_binary_frame(data);
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        break;

    default:
        break;
    }
}

int ws_client_init(void) {
    esp_websocket_client_config_t cfg = {
        .uri = s_ws_server_url,
        .network_timeout_ms = WS_TIMEOUT_MS,
        .buffer_size = WS_BUFFER_SIZE,
        .task_stack = WS_TASK_STACK,
    };

    if (s_ws_client != NULL) {
        return 0;
    }

    s_ws_client = esp_websocket_client_init(&cfg);
    if (s_ws_client == NULL) {
        ESP_LOGE(TAG, "failed to init WebSocket client");
        return -1;
    }

    if (s_ws_send_lock == NULL) {
        s_ws_send_lock = xSemaphoreCreateMutex();
        if (s_ws_send_lock == NULL) {
            esp_websocket_client_destroy(s_ws_client);
            s_ws_client = NULL;
            ESP_LOGE(TAG, "failed to create WebSocket send lock");
            return -1;
        }
    }

    ws_reset_session_state();
    esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    ESP_LOGI(TAG, "WebSocket client initialized (URL: %s)", s_ws_server_url);
    return 0;
}

int ws_client_set_server_url(const char *url) {
    if (url == NULL || strlen(url) >= WS_URL_MAX_LEN) {
        ESP_LOGE(TAG, "invalid URL or URL too long");
        return -1;
    }
    if (s_ws_client != NULL) {
        ESP_LOGW(TAG, "cannot set URL after client initialized");
        return -1;
    }

    strncpy(s_ws_server_url, url, WS_URL_MAX_LEN - 1);
    s_ws_server_url[WS_URL_MAX_LEN - 1] = '\0';
    ESP_LOGI(TAG, "server URL set to: %s", s_ws_server_url);
    return 0;
}

const char *ws_client_get_server_url(void) {
    return s_ws_server_url;
}

int ws_client_start(void) {
    esp_err_t ret;

    if (s_ws_client == NULL) {
        ESP_LOGE(TAG, "WebSocket not initialized");
        return -1;
    }

    ret = esp_websocket_client_start(s_ws_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to start WebSocket: %s", esp_err_to_name(ret));
        return -1;
    }

    return 0;
}

void ws_client_stop(void) {
    if (camera_service_is_streaming()) {
        camera_service_stop_stream();
    }

    ws_abort_tts_playback();
    if (s_ws_client != NULL) {
        esp_websocket_client_stop(s_ws_client);
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
    }

    ws_reset_session_state();
}

int ws_client_send_binary(const uint8_t *data, int len) {
    if (data == NULL || len < 0) {
        return -1;
    }

    return ws_client_lock_and_send(true, data, len, false);
}

int ws_client_send_text(const char *text) {
    return ws_send_text_internal(text, false);
}

int ws_client_is_connected(void) {
    return s_socket_connected ? 1 : 0;
}

int ws_client_is_session_ready(void) {
    return (s_socket_connected && s_hello_acknowledged) ? 1 : 0;
}

void ws_client_mark_server_response(void) {
    s_waiting_for_response = false;
}

void ws_client_mark_hello_acked(void) {
    if (!s_socket_connected || s_hello_acknowledged) {
        return;
    }

    s_hello_acknowledged = true;
    display_update(NULL, "happy", 0, NULL);
    if (ws_send_device_firmware() < 0) {
        ESP_LOGW(TAG, "failed to send evt.device.firmware");
    }
}

int ws_send_sys_pong(void) {
    cJSON *data = cJSON_CreateObject();

    return ws_send_json_envelope("sys.pong", 0, data, true);
}

int ws_send_sys_ack(const char *message_type, const char *command_id) {
    cJSON *data = cJSON_CreateObject();

    if (data == NULL || message_type == NULL) {
        cJSON_Delete(data);
        return -1;
    }

    cJSON_AddStringToObject(data, "type", message_type);
    if (command_id != NULL && command_id[0] != '\0') {
        cJSON_AddStringToObject(data, "command_id", command_id);
    }

    return ws_send_json_envelope("sys.ack", 0, data, true);
}

int ws_send_sys_nack(const char *message_type, const char *command_id, const char *reason) {
    cJSON *data = cJSON_CreateObject();

    if (data == NULL || message_type == NULL || reason == NULL) {
        cJSON_Delete(data);
        return -1;
    }

    cJSON_AddStringToObject(data, "type", message_type);
    if (command_id != NULL && command_id[0] != '\0') {
        cJSON_AddStringToObject(data, "command_id", command_id);
    }
    cJSON_AddStringToObject(data, "reason", reason);

    return ws_send_json_envelope("sys.nack", 1, data, true);
}

int ws_send_device_firmware(void) {
    cJSON *data = cJSON_CreateObject();
    char mac[18];

    if (data == NULL) {
        return -1;
    }

    ws_get_mac_string(mac, sizeof(mac));
    cJSON_AddStringToObject(data, "fw_version", ota_service_get_fw_version());
    cJSON_AddStringToObject(data, "board_model", "WatcheRobot-S3");
    if (mac[0] != '\0') {
        cJSON_AddStringToObject(data, "mac", mac);
    }

    return ws_send_json_envelope("evt.device.firmware", 0, data, false);
}

int ws_send_device_error(int code, const char *message) {
    cJSON *data = cJSON_CreateObject();

    if (data == NULL || message == NULL || message[0] == '\0') {
        cJSON_Delete(data);
        return -1;
    }

    cJSON_AddStringToObject(data, "message", message);
    return ws_send_json_envelope("evt.device.error", code > 0 ? code : WS_DEVICE_ERROR_CODE_GENERIC, data, false);
}

int ws_send_ota_progress(int progress, const char *state, const char *message) {
    cJSON *data = cJSON_CreateObject();

    if (data == NULL) {
        return -1;
    }

    cJSON_AddNumberToObject(data, "progress", progress);
    if (state != NULL && state[0] != '\0') {
        cJSON_AddStringToObject(data, "state", state);
    }
    if (message != NULL && message[0] != '\0') {
        cJSON_AddStringToObject(data, "message", message);
    }

    return ws_send_json_envelope("evt.ota.progress", 0, data, false);
}

int ws_send_ota_handshake(const char *transfer_id, const char *status) {
    cJSON *data = cJSON_CreateObject();
    char mac[18];

    if (data == NULL) {
        return -1;
    }

    ws_get_mac_string(mac, sizeof(mac));
    if (transfer_id != NULL && transfer_id[0] != '\0') {
        cJSON_AddStringToObject(data, "transfer_id", transfer_id);
    }
    cJSON_AddStringToObject(data, "fw_version", ota_service_get_fw_version());
    cJSON_AddStringToObject(data, "board_model", "WatcheRobot-S3");
    if (status != NULL && status[0] != '\0') {
        cJSON_AddStringToObject(data, "status", status);
    }
    if (mac[0] != '\0') {
        cJSON_AddStringToObject(data, "mac", mac);
    }

    return ws_send_json_envelope("xfer.ota.handshake", 0, data, false);
}

int ws_send_servo_position(float x_deg, float y_deg) {
    cJSON *data = cJSON_CreateObject();

    if (data == NULL) {
        return -1;
    }

    cJSON_AddNumberToObject(data, "x_deg", x_deg);
    cJSON_AddNumberToObject(data, "y_deg", y_deg);
    return ws_send_json_envelope("evt.servo.position", 0, data, false);
}

int ws_send_camera_state(const char *action, const char *state, int fps, const char *message) {
    cJSON *data = cJSON_CreateObject();

    if (data == NULL || action == NULL || state == NULL) {
        cJSON_Delete(data);
        return -1;
    }

    cJSON_AddStringToObject(data, "action", action);
    cJSON_AddStringToObject(data, "state", state);
    if (fps > 0) {
        cJSON_AddNumberToObject(data, "fps", fps);
    }
    if (message != NULL && message[0] != '\0') {
        cJSON_AddStringToObject(data, "message", message);
    }

    return ws_send_json_envelope("evt.camera.state", 0, data, false);
}

int ws_send_video_frame(const uint8_t *jpeg, size_t len, bool first_frame) {
    uint8_t flags = WS_FRAME_FLAG_KEYFRAME;

    if (jpeg == NULL || len == 0U) {
        return -1;
    }
    if (first_frame) {
        flags |= WS_FRAME_FLAG_FIRST;
    }

    return ws_send_binary_packet(WS_FRAME_TYPE_VIDEO, flags, jpeg, len);
}

int ws_send_video_end(void) {
    return ws_send_binary_packet(WS_FRAME_TYPE_VIDEO, WS_FRAME_FLAG_LAST, NULL, 0);
}

int ws_send_image_frame(const uint8_t *jpeg, size_t len) {
    if (jpeg == NULL || len == 0U) {
        return -1;
    }

    return ws_send_binary_packet(WS_FRAME_TYPE_IMAGE,
                                 WS_FRAME_FLAG_FIRST | WS_FRAME_FLAG_LAST | WS_FRAME_FLAG_KEYFRAME,
                                 jpeg,
                                 len);
}

void ws_client_get_media_send_stats(ws_client_media_send_stats_t *stats) {
    if (stats != NULL) {
        *stats = s_last_media_send_stats;
    }
}

int ws_send_audio(const uint8_t *data, int len) {
    uint8_t flags = WS_FRAME_FLAG_NONE;
    int ret;

    if (data == NULL || len <= 0) {
        return -1;
    }

    if (!s_audio_upload_active) {
        flags |= WS_FRAME_FLAG_FIRST;
    }

    ret = ws_send_binary_packet(WS_FRAME_TYPE_AUDIO, flags, data, (size_t)len);
    if (ret == 0) {
        s_audio_upload_active = true;
    }

    return ret;
}

int ws_send_audio_end(void) {
    uint8_t flags = WS_FRAME_FLAG_LAST;
    int ret;

    if (!s_audio_upload_active) {
        flags |= WS_FRAME_FLAG_FIRST;
    }

    ret = ws_send_binary_packet(WS_FRAME_TYPE_AUDIO, flags, NULL, 0);
    s_audio_upload_active = false;
    if (ret == 0) {
        s_waiting_for_response = true;
        s_timeout_display_count = 0;
        s_response_wait_start_time = esp_timer_get_time();
        ESP_LOGI(TAG, "audio end sent, waiting for server response (%dms)", WS_RESPONSE_TIMEOUT_MS);
    }

    return ret;
}

void ws_handle_tts_binary(const uint8_t *data, int len) {
    int written;

    if (data == NULL || len <= 0) {
        return;
    }

    if (!s_tts_playing) {
        ESP_LOGI(TAG, "TTS started, first chunk: %d bytes", len);
        s_waiting_for_response = false;
        display_update("", "speaking", 0, NULL);

#ifdef CONFIG_ENABLE_WAKE_WORD
        voice_recorder_pause_wake_word();
#endif

        hal_audio_set_playback_mode(true);
        hal_audio_set_sample_rate(24000);
        hal_audio_start();
        s_tts_playing = true;
    }

    written = hal_audio_write(data, len);
    if (written != len) {
        ESP_LOGW(TAG, "TTS playback incomplete: %d/%d", written, len);
    }
}

void ws_tts_complete(void) {
    s_waiting_for_response = false;

    if (s_tts_playing) {
        ESP_LOGI(TAG, "TTS playback complete");
        vTaskDelay(pdMS_TO_TICKS(500));
        hal_audio_stop();
        hal_audio_set_playback_mode(false);
        vTaskDelay(pdMS_TO_TICKS(1000));
        hal_audio_set_sample_rate(16000);
        display_update(NULL, "happy", 0, NULL);
        s_tts_playing = false;
    }

    ws_resume_wake_word_after_tts();
}

void ws_tts_timeout_check(void) {
#ifdef CONFIG_ENABLE_WAKE_WORD
    if (s_waiting_for_response) {
        int64_t elapsed_ms = (esp_timer_get_time() - s_response_wait_start_time) / 1000;
        if (elapsed_ms > WS_RESPONSE_TIMEOUT_MS && s_timeout_display_count < 1) {
            ESP_LOGW(TAG, "response timeout (%lld ms), resuming wake word detection", elapsed_ms);
            s_waiting_for_response = false;
            voice_recorder_resume_wake_word();
            display_update("Timeout", "error", 0, NULL);
            s_timeout_display_count++;
        }
    }
#endif
}
