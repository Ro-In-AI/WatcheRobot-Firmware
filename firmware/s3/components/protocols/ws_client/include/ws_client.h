#ifndef WS_CLIENT_H
#define WS_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file ws_client.h
 * @brief WebSocket client interface (Watcher protocol v0.1.5)
 */

typedef enum {
    WS_FRAME_TYPE_AUDIO = 1,
    WS_FRAME_TYPE_VIDEO = 2,
    WS_FRAME_TYPE_IMAGE = 3,
    WS_FRAME_TYPE_OTA = 4,
} ws_frame_type_t;

typedef enum {
    WS_FRAME_FLAG_NONE = 0,
    WS_FRAME_FLAG_FIRST = 1 << 0,
    WS_FRAME_FLAG_LAST = 1 << 1,
    WS_FRAME_FLAG_KEYFRAME = 1 << 2,
    WS_FRAME_FLAG_FRAGMENT = 1 << 3,
} ws_frame_flag_t;

typedef struct {
    bool valid;
    bool binary;
    uint8_t frame_type;
    size_t payload_len;
    uint32_t packet_len;
    uint32_t lock_wait_us;
    uint32_t send_us;
    uint32_t total_us;
    uint64_t timestamp_us;
} ws_client_media_send_stats_t;

typedef struct {
    bool valid;
    uint32_t queued_frames;
    uint32_t sent_frames;
    uint32_t dropped_frames;
    uint32_t pending_frames;
    uint32_t high_watermark;
    uint32_t last_queue_delay_us;
    bool session_open;
    bool first_frame_pending;
    bool end_pending;
} ws_client_audio_queue_stats_t;

/**
 * Initialize WebSocket client
 */
int ws_client_init(void);

/**
 * Set server URL (before ws_client_start)
 * @param url WebSocket URL (e.g., "ws://192.168.1.100:8765")
 * @return 0 on success, -1 on error
 */
int ws_client_set_server_url(const char *url);

/**
 * Get current server URL
 * @return Server URL string (static buffer, do not free)
 */
const char *ws_client_get_server_url(void);

/**
 * Start WebSocket connection
 */
int ws_client_start(void);

/**
 * Stop WebSocket connection
 */
void ws_client_stop(void);

/**
 * Send binary data via WebSocket
 * @param data Data buffer
 * @param len Data length
 * @return Bytes sent on success, -1 on error
 */
int ws_client_send_binary(const uint8_t *data, int len);

/**
 * Send text message via WebSocket
 * @param text Text message
 * @return Bytes sent on success, -1 on error
 */
int ws_client_send_text(const char *text);

/**
 * Check if WebSocket is connected
 */
int ws_client_is_connected(void);

/**
 * Check if the WebSocket session is ready for business traffic.
 */
int ws_client_is_session_ready(void);

/**
 * Mark that the server has responded to the current voice session.
 */
void ws_client_mark_server_response(void);

/**
 * Mark the hello handshake as acknowledged.
 */
void ws_client_mark_hello_acked(void);

/**
 * Send sys.pong in response to a server ping.
 */
int ws_send_sys_pong(void);

/**
 * Send sys.ack for an accepted control command.
 */
int ws_send_sys_ack(const char *message_type, const char *command_id);

/**
 * Send sys.nack for a rejected control command.
 */
int ws_send_sys_nack(const char *message_type, const char *command_id, const char *reason);

/**
 * Send the current device firmware metadata.
 */
int ws_send_device_firmware(void);

/**
 * Send a device error event to desktop observers.
 */
int ws_send_device_error(int code, const char *message);

/**
 * Send OTA progress to desktop observers.
 */
int ws_send_ota_progress(int progress, const char *state, const char *message);

/**
 * Send OTA handshake metadata for the current firmware.
 */
int ws_send_ota_handshake(const char *transfer_id, const char *status);

/**
 * Send the current servo position.
 */
int ws_send_servo_position(float x_deg, float y_deg);

/**
 * Send a camera state event.
 */
int ws_send_camera_state(const char *action, const char *state, int fps, const char *message);

/**
 * Send one MJPEG video frame using the WSPK binary header.
 */
int ws_send_video_frame(const uint8_t *jpeg, size_t len, bool first_frame);

/**
 * Send the terminal marker of a video stream using a zero-payload WSPK frame.
 */
int ws_send_video_end(void);

/**
 * Send one JPEG image using the WSPK binary header.
 */
int ws_send_image_frame(const uint8_t *jpeg, size_t len);

/**
 * Get the most recent media send timing sample.
 */
void ws_client_get_media_send_stats(ws_client_media_send_stats_t *stats);

/**
 * Send audio data via WebSocket using WSPK audio frames.
 * @param data Audio data (PCM 16-bit, 16kHz, mono, LE)
 * @param len Data length
 * @note This call only enqueues audio; a worker task performs the actual send.
 * @return 0 on success, -1 on error
 */
int ws_send_audio(const uint8_t *data, int len);

/**
 * Send audio end marker via WebSocket using a zero-payload LAST audio frame.
 * @return 0 on success, -1 on error
 */
int ws_send_audio_end(void);

/**
 * Handle TTS binary frame from WebSocket.
 * @param data Binary frame data (PCM 16-bit, 24kHz, mono, LE)
 * @param len Frame length
 */
void ws_handle_tts_binary(const uint8_t *data, int len);

/**
 * Signal TTS playback complete
 * Call this when the LAST audio frame is received.
 */
void ws_tts_complete(void);

/**
 * Check response timeout and recover wake-word mode if needed.
 */
void ws_tts_timeout_check(void);

/**
 * Get the current audio queue statistics.
 */
void ws_client_get_audio_queue_stats(ws_client_audio_queue_stats_t *stats);

#endif /* WS_CLIENT_H */
