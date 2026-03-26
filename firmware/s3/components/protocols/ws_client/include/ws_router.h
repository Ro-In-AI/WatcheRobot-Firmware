#ifndef WS_ROUTER_H
#define WS_ROUTER_H

#include <stdbool.h>

typedef enum {
    WS_MSG_UNKNOWN = 0,
    WS_MSG_SYS_ACK,
    WS_MSG_SYS_NACK,
    WS_MSG_SYS_PING,
    WS_MSG_SYS_PONG,
    WS_MSG_SYS_SESSION_RESUME,
    WS_MSG_CTRL_SERVO_ANGLE,
    WS_MSG_CTRL_ROBOT_STATE_SET,
    WS_MSG_CTRL_CAMERA_VIDEO_CONFIG,
    WS_MSG_CTRL_CAMERA_CAPTURE_IMAGE,
    WS_MSG_CTRL_CAMERA_START_VIDEO,
    WS_MSG_CTRL_CAMERA_STOP_VIDEO,
    WS_MSG_EVT_ASR_RESULT,
    WS_MSG_EVT_AI_STATUS,
    WS_MSG_EVT_AI_THINKING,
    WS_MSG_EVT_AI_REPLY,
    WS_MSG_XFER_OTA_HANDSHAKE,
    WS_MSG_XFER_OTA_CHECKSUM,
} ws_msg_type_t;

#define WS_MESSAGE_TYPE_MAX 48
#define WS_COMMAND_ID_MAX 48
#define WS_TEXT_DATA_MAX 256
#define WS_STATUS_MAX 64
#define WS_KIND_MAX 32
#define WS_RESOURCE_NAME_MAX 96
#define WS_CAPTURE_ACTION_MAX 16
#define WS_TRANSFER_ID_MAX 64
#define WS_SHA256_MAX 80
#define WS_STATE_ID_MAX 32

typedef struct {
    char type[WS_MESSAGE_TYPE_MAX];
    char command_id[WS_COMMAND_ID_MAX];
    char message[WS_TEXT_DATA_MAX];
    int code;
} ws_sys_ack_t;

typedef struct {
    char type[WS_MESSAGE_TYPE_MAX];
    char command_id[WS_COMMAND_ID_MAX];
    char reason[WS_TEXT_DATA_MAX];
    int code;
} ws_sys_nack_t;

typedef struct {
    bool has_x;
    bool has_y;
    float x_deg;
    float y_deg;
    int duration_ms;
} ws_servo_cmd_t;

typedef struct {
    char command_id[WS_COMMAND_ID_MAX];
    char state_id[WS_STATE_ID_MAX];
} ws_state_cmd_t;

typedef struct {
    char text[WS_TEXT_DATA_MAX];
} ws_text_event_t;

typedef struct {
    char status[WS_STATUS_MAX];
    char message[WS_TEXT_DATA_MAX];
    char image_name[WS_RESOURCE_NAME_MAX];
    char action_file[WS_RESOURCE_NAME_MAX];
    char sound_file[WS_RESOURCE_NAME_MAX];
} ws_ai_status_t;

typedef struct {
    char kind[WS_KIND_MAX];
    char content[WS_TEXT_DATA_MAX];
} ws_ai_thinking_t;

typedef struct {
    char command_id[WS_COMMAND_ID_MAX];
    char action[WS_CAPTURE_ACTION_MAX];
    int width;
    int height;
    int fps;
    int quality;
} ws_capture_cmd_t;

typedef struct {
    char message_type[WS_MESSAGE_TYPE_MAX];
    char transfer_id[WS_TRANSFER_ID_MAX];
    char sha256[WS_SHA256_MAX];
} ws_transfer_cmd_t;

typedef void (*ws_sys_ack_handler_t)(const ws_sys_ack_t *msg);
typedef void (*ws_sys_nack_handler_t)(const ws_sys_nack_t *msg);
typedef void (*ws_sys_void_handler_t)(void);
typedef void (*ws_servo_handler_t)(const ws_servo_cmd_t *cmd);
typedef void (*ws_state_handler_t)(const ws_state_cmd_t *cmd);
typedef void (*ws_text_event_handler_t)(const ws_text_event_t *event);
typedef void (*ws_ai_status_handler_t)(const ws_ai_status_t *event);
typedef void (*ws_ai_thinking_handler_t)(const ws_ai_thinking_t *event);
typedef void (*ws_capture_handler_t)(const ws_capture_cmd_t *cmd);
typedef void (*ws_transfer_handler_t)(const ws_transfer_cmd_t *cmd);

typedef struct {
    ws_sys_ack_handler_t on_sys_ack;
    ws_sys_nack_handler_t on_sys_nack;
    ws_sys_void_handler_t on_sys_ping;
    ws_sys_void_handler_t on_sys_pong;
    ws_sys_void_handler_t on_session_resume;
    ws_servo_handler_t on_servo;
    ws_state_handler_t on_state_set;
    ws_capture_handler_t on_capture;
    ws_text_event_handler_t on_asr_result;
    ws_ai_status_handler_t on_ai_status;
    ws_ai_thinking_handler_t on_ai_thinking;
    ws_text_event_handler_t on_ai_reply;
    ws_transfer_handler_t on_transfer;
} ws_router_t;

void ws_router_init(ws_router_t *router);
ws_msg_type_t ws_route_message(const char *json_str);

#endif /* WS_ROUTER_H */
