#ifndef WS_HANDLERS_H
#define WS_HANDLERS_H

#include "ws_router.h"

/**
 * @file ws_handlers.h
 * @brief WebSocket message handlers for Watcher hardware (protocol v0.1.1)
 */

void ws_handlers_init(void);

void on_sys_ack_handler(const ws_sys_ack_t *msg);
void on_sys_nack_handler(const ws_sys_nack_t *msg);
void on_sys_ping_handler(void);
void on_sys_pong_handler(void);
void on_session_resume_handler(void);
void on_servo_handler(const ws_servo_cmd_t *cmd);
void on_capture_handler(const ws_capture_cmd_t *cmd);
void on_asr_result_handler(const ws_text_event_t *event);
void on_ai_status_handler(const ws_ai_status_t *event);
void on_ai_thinking_handler(const ws_ai_thinking_t *event);
void on_ai_reply_handler(const ws_text_event_t *event);
void on_transfer_handler(const ws_transfer_cmd_t *cmd);

const char *ws_ai_status_to_emoji(const char *status, const char *message);
ws_router_t ws_handlers_get_router(void);

#endif /* WS_HANDLERS_H */
