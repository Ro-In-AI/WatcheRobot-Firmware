#ifndef WS_HANDLERS_H
#define WS_HANDLERS_H

#include "ws_router.h"

/**
 * @file ws_handlers.h
 * @brief WebSocket message handlers for Watcher device (Protocol v2.0)
 *
 * These handlers are called by ws_router when receiving messages from cloud.
 * Each handler performs the appropriate action (UART, display, etc.)
 */

/* ------------------------------------------------------------------ */
/* Handler Functions (to be registered with ws_router)                */
/* ------------------------------------------------------------------ */

/**
 * Handle servo command - forward to MCU via UART
 * @param cmd Servo command with x, y angles
 */
void on_servo_handler(const ws_servo_cmd_t *cmd);

/**
 * Handle display command - update screen text and emoji
 * @param cmd Display command with text, emoji, size
 */
void on_display_handler(const ws_display_cmd_t *cmd);

/**
 * Handle status command - update display based on status data
 * @param cmd Status command with data string
 */
void on_status_handler(const ws_status_cmd_t *cmd);

/**
 * Handle capture command - take photo (MVP: not implemented)
 * @param cmd Capture command with quality
 */
void on_capture_handler(const ws_capture_cmd_t *cmd);

/**
 * Handle reboot command - restart device
 */
void on_reboot_handler(void);

/* ------------------------------------------------------------------ */
/* New Handlers - Protocol v2.0                                       */
/* ------------------------------------------------------------------ */

/**
 * Handle ASR result - display recognized text
 * @param cmd ASR result with recognized text
 */
void on_asr_result_handler(const ws_asr_result_cmd_t *cmd);

/**
 * Handle bot reply - display AI response (optional)
 * @param cmd Bot reply with AI text
 */
void on_bot_reply_handler(const ws_bot_reply_cmd_t *cmd);

/**
 * Handle TTS end - stop audio playback and switch to happy
 */
void on_tts_end_handler(void);

/**
 * Handle error message - display error state
 * @param cmd Error command with code and message
 */
void on_error_handler(const ws_error_cmd_t *cmd);

/* ------------------------------------------------------------------ */
/* Helper Functions (for testing)                                     */
/* ------------------------------------------------------------------ */

/**
 * Parse status data string to determine emoji
 * @param data Status data string (may contain [thinking], etc.)
 * @return Emoji string (analyzing, speaking, standby, sad)
 */
const char* ws_status_data_to_emoji(const char *data);

/**
 * Get all handlers as a router struct (convenience function)
 * @return ws_router_t with all handlers populated
 */
ws_router_t ws_handlers_get_router(void);

#endif /* WS_HANDLERS_H */
