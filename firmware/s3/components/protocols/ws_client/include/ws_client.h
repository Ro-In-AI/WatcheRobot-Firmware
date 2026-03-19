#ifndef WS_CLIENT_H
#define WS_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file ws_client.h
 * @brief WebSocket client interface (Protocol v2.0)
 */

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
 * Send sys.ack for an accepted control command.
 */
int ws_send_sys_ack(const char *command_id, const char *command_type, uint16_t stream_id, const char *message);

/**
 * Send sys.nack for a rejected control command.
 */
int ws_send_sys_nack(const char *command_id, const char *command_type, const char *reason);

/**
 * Send a camera state event.
 */
int ws_send_camera_state(const char *action, const char *state, uint16_t stream_id, int fps, const char *message);

/**
 * Send one MJPEG video frame using the WSPK binary header.
 */
int ws_send_video_frame(const uint8_t *jpeg, size_t len, uint16_t stream_id, uint32_t seq, bool first_frame);

/**
 * Send the terminal marker of a video stream using a zero-payload WSPK frame.
 */
int ws_send_video_end(uint16_t stream_id, uint32_t seq);

/**
 * Send one JPEG image using the WSPK binary header.
 */
int ws_send_image_frame(const uint8_t *jpeg, size_t len, uint16_t stream_id);

/**
 * Get the most recent media send timing sample.
 */
void ws_client_get_media_send_stats(ws_client_media_send_stats_t *stats);

/**
 * Send audio data via WebSocket (v2.0: raw PCM)
 * @param data Audio data (PCM 16-bit, 16kHz, mono)
 * @param len Data length
 * @return 0 on success, -1 on error
 */
int ws_send_audio(const uint8_t *data, int len);

/**
 * Send audio end marker via WebSocket (v2.0: "over")
 * @return 0 on success, -1 on error
 */
int ws_send_audio_end(void);

/**
 * Handle TTS binary frame from WebSocket (v2.0: raw PCM)
 * @param data Binary frame data (PCM 16-bit, 24kHz, mono)
 * @param len Frame length
 */
void ws_handle_tts_binary(const uint8_t *data, int len);

/**
 * Signal TTS playback complete
 * Call this when tts_end message is received
 */
void ws_tts_complete(void);

/**
 * Check TTS timeout and auto-complete if needed
 * Note: In v2.0, this is a no-op (tts_end message is used instead)
 */
void ws_tts_timeout_check(void);

#endif /* WS_CLIENT_H */
