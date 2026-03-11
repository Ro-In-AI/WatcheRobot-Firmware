#ifndef WS_ROUTER_H
#define WS_ROUTER_H

#include <stdint.h>
#include <stdbool.h>

/* Message types (Protocol v2.0) */
typedef enum {
    WS_MSG_UNKNOWN = 0,

    /* Control commands (Cloud -> Watcher) - v2.0 format */
    WS_MSG_SERVO,           /* {"type": "servo", "data": {"id": "x", "angle": 90, "time": 500}} */
    WS_MSG_DISPLAY,         /* {"type": "display", "code": 0, "data": {"text": "...", "emoji": "happy"}} */
    WS_MSG_CAPTURE,         /* {"type": "capture", "code": 0, "data": {"quality": 80}} */
    WS_MSG_STATUS,          /* {"type": "status", "code": 0, "data": "状态描述"} */
    WS_MSG_REBOOT,          /* {"type": "reboot", "code": 0, "data": null} */

    /* New message types - v2.0 */
    WS_MSG_ASR_RESULT,      /* {"type": "asr_result", "code": 0, "data": "识别文本"} */
    WS_MSG_BOT_REPLY,       /* {"type": "bot_reply", "code": 0, "data": "AI回复"} */
    WS_MSG_TTS_END,         /* {"type": "tts_end", "code": 0, "data": "ok"} */
    WS_MSG_ERROR_MSG,       /* {"type": "error", "code": 1, "data": "错误描述"} */

    /* Media streams (Watcher -> Cloud) */
    WS_MSG_AUDIO,           /* Binary PCM 16kHz */
    WS_MSG_AUDIO_END,       /* "over" text */
    WS_MSG_VIDEO,           /* {"type": "video", ...} */
    WS_MSG_SENSOR,          /* {"type": "sensor", ...} */

    /* System messages */
    WS_MSG_PING,
    WS_MSG_PONG,
    WS_MSG_ERROR,
    WS_MSG_CONNECTED,

    /* Legacy (deprecated) */
    WS_MSG_TTS,             /* Use binary frames instead */
    WS_MSG_STATUS_OLD,      /* Old format with state/message fields */
} ws_msg_type_t;

/* Servo command structure (v2.1 format) */
#define WS_SERVO_ID_MAX 8
typedef struct {
    char id[WS_SERVO_ID_MAX];   /* "x" or "y" - servo identifier */
    int angle;                  /* 0-180 */
    int time_ms;               /* movement duration in ms */
} ws_servo_cmd_t;

/* Display command structure */
#define WS_DISPLAY_TEXT_MAX  128
#define WS_DISPLAY_EMOJI_MAX 16
typedef struct {
    char text[WS_DISPLAY_TEXT_MAX];   /* text to display */
    char emoji[WS_DISPLAY_EMOJI_MAX]; /* happy/sad/surprised/angry/normal (optional) */
    int size;                         /* font size (default 24) */
} ws_display_cmd_t;

/* Status command structure (v2.0 - data is string) */
#define WS_STATUS_DATA_MAX 256
typedef struct {
    char data[WS_STATUS_DATA_MAX];  /* status description string */
} ws_status_cmd_t;

/* ASR result structure (v2.0) */
#define WS_TEXT_DATA_MAX 256
typedef struct {
    char text[WS_TEXT_DATA_MAX];  /* recognized text */
} ws_asr_result_cmd_t;

/* Bot reply structure (v2.0) */
typedef struct {
    char text[WS_TEXT_DATA_MAX];  /* AI reply text */
} ws_bot_reply_cmd_t;

/* Error message structure (v2.0) */
typedef struct {
    int code;                       /* error code (non-zero) */
    char message[WS_TEXT_DATA_MAX]; /* error description */
} ws_error_cmd_t;

/* Capture command structure */
typedef struct {
    int quality;            /* JPEG quality (1-100) */
} ws_capture_cmd_t;

/* Legacy structures (deprecated) */
typedef struct {
    char format[16];
    const char *data_b64;
    int data_len;
} ws_tts_cmd_t;

typedef struct {
    char state[16];
    char message[256];
} ws_status_old_cmd_t;

/* Handler function types (to be implemented by application) */
typedef void (*ws_servo_handler_t)(const ws_servo_cmd_t *cmd);
typedef void (*ws_display_handler_t)(const ws_display_cmd_t *cmd);
typedef void (*ws_status_handler_t)(const ws_status_cmd_t *cmd);
typedef void (*ws_capture_handler_t)(const ws_capture_cmd_t *cmd);
typedef void (*ws_reboot_handler_t)(void);

/* New handler types - v2.0 */
typedef void (*ws_asr_result_handler_t)(const ws_asr_result_cmd_t *cmd);
typedef void (*ws_bot_reply_handler_t)(const ws_bot_reply_cmd_t *cmd);
typedef void (*ws_tts_end_handler_t)(void);
typedef void (*ws_error_handler_t)(const ws_error_cmd_t *cmd);

/* Router context */
typedef struct {
    ws_servo_handler_t   on_servo;
    ws_display_handler_t on_display;
    ws_status_handler_t  on_status;
    ws_capture_handler_t on_capture;
    ws_reboot_handler_t  on_reboot;

    /* New handlers - v2.0 */
    ws_asr_result_handler_t on_asr_result;
    ws_bot_reply_handler_t  on_bot_reply;
    ws_tts_end_handler_t    on_tts_end;
    ws_error_handler_t      on_error;
} ws_router_t;

/**
 * Initialize router with handlers
 */
void ws_router_init(ws_router_t *router);

/**
 * Route a JSON message to appropriate handler (v2.0 format)
 * @param json_str Null-terminated JSON string
 * @return Message type, or WS_MSG_UNKNOWN on parse error
 */
ws_msg_type_t ws_route_message(const char *json_str);

/**
 * Parse servo command from JSON (v2.1 format)
 * @param json_str JSON string
 * @param out_cmd Output structure
 * @return 0 on success, -1 on error
 */
int ws_parse_servo(const char *json_str, ws_servo_cmd_t *out_cmd);

/**
 * Parse display command from JSON
 * @param json_str JSON string
 * @param out_cmd Output structure
 * @return 0 on success, -1 on error
 */
int ws_parse_display(const char *json_str, ws_display_cmd_t *out_cmd);

/**
 * Parse status command from JSON (v2.0 format)
 * @param json_str JSON string
 * @param out_cmd Output structure
 * @return 0 on success, -1 on error
 */
int ws_parse_status(const char *json_str, ws_status_cmd_t *out_cmd);

/**
 * Parse ASR result from JSON (v2.0 format)
 * @param json_str JSON string
 * @param out_cmd Output structure
 * @return 0 on success, -1 on error
 */
int ws_parse_asr_result(const char *json_str, ws_asr_result_cmd_t *out_cmd);

/**
 * Parse bot reply from JSON (v2.0 format)
 * @param json_str JSON string
 * @param out_cmd Output structure
 * @return 0 on success, -1 on error
 */
int ws_parse_bot_reply(const char *json_str, ws_bot_reply_cmd_t *out_cmd);

/**
 * Parse error message from JSON (v2.0 format)
 * @param json_str JSON string
 * @param out_cmd Output structure
 * @return 0 on success, -1 on error
 */
int ws_parse_error(const char *json_str, ws_error_cmd_t *out_cmd);

#endif /* WS_ROUTER_H */
