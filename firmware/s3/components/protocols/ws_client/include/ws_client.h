#ifndef WS_CLIENT_H
#define WS_CLIENT_H

#include <stdint.h>

/**
 * @file ws_client.h
 * @brief WebSocket client interface (Protocol v2.0)
 */

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
const char* ws_client_get_server_url(void);

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
