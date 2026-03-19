/**
 * @file ws_router.c
 * @brief WebSocket message router implementation (Protocol v2.1)
 */

#include "ws_router.h"
#include "cJSON.h"
#include <limits.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Private: Router context                                            */
/* ------------------------------------------------------------------ */

static ws_router_t g_router = {0};

/* ------------------------------------------------------------------ */
/* Public: Initialize router                                          */
/* ------------------------------------------------------------------ */

void ws_router_init(ws_router_t *router) {
    if (router) {
        g_router = *router;
    }
}

/* ------------------------------------------------------------------ */
/* Private: Get string from cJSON object safely                       */
/* ------------------------------------------------------------------ */

static const char *get_string(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsString(item)) {
        return item->valuestring;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Private: Get integer from cJSON object safely                      */
/* ------------------------------------------------------------------ */

static int get_int(cJSON *obj, const char *key, int default_val) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return default_val;
}

/* ------------------------------------------------------------------ */
/* Private: Copy string to fixed buffer safely                        */
/* ------------------------------------------------------------------ */

static void copy_string(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0)
        return;
    if (src) {
        strncpy(dst, src, dst_size - 1);
        dst[dst_size - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

static void fill_camera_cmd(cJSON *data, const char *action, ws_capture_cmd_t *cmd) {
    if (!cmd) {
        return;
    }

    memset(cmd, 0, sizeof(*cmd));
    copy_string(cmd->action, sizeof(cmd->action), action);
    if (data && cJSON_IsObject(data)) {
        copy_string(cmd->command_id, sizeof(cmd->command_id), get_string(data, "command_id"));
        cmd->width = get_int(data, "width", 0);
        cmd->height = get_int(data, "height", 0);
        cmd->fps = get_int(data, "fps", 0);
        cmd->quality = get_int(data, "quality", 0);
    }
}

/* ------------------------------------------------------------------ */
/* Public: Route message to appropriate handler (v2.1 format)          */
/* ------------------------------------------------------------------ */

ws_msg_type_t ws_route_message(const char *json_str) {
    if (!json_str) {
        return WS_MSG_UNKNOWN;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        return WS_MSG_UNKNOWN;
    }

    /* Get type field */
    const char *type = get_string(root, "type");
    if (!type) {
        cJSON_Delete(root);
        return WS_MSG_UNKNOWN;
    }

    ws_msg_type_t msg_type = WS_MSG_UNKNOWN;

    /* Route based on type */
    if (strcmp(type, "servo") == 0) {
        msg_type = WS_MSG_SERVO;
        if (g_router.on_servo) {
            /* v2.1 format: data.id, data.angle, data.time */
            cJSON *data = cJSON_GetObjectItem(root, "data");
            if (data && cJSON_IsObject(data)) {
                ws_servo_cmd_t cmd = {0};
                copy_string(cmd.id, sizeof(cmd.id), get_string(data, "id"));
                int angle1 = get_int(data, "angle", INT_MIN);
                int angle2 = get_int(data, "Angle", INT_MIN);
                cmd.angle = (angle1 != INT_MIN) ? angle1 : angle2;
                if (cmd.angle == INT_MIN)
                    cmd.angle = 90;
                cmd.time_ms = get_int(data, "time", 100);
                g_router.on_servo(&cmd);
            }
        }
    } else if (strcmp(type, "display") == 0) {
        msg_type = WS_MSG_DISPLAY;
        if (g_router.on_display) {
            cJSON *data = cJSON_GetObjectItem(root, "data");
            if (data && cJSON_IsObject(data)) {
                ws_display_cmd_t cmd = {0};
                copy_string(cmd.text, sizeof(cmd.text), get_string(data, "text"));
                copy_string(cmd.emoji, sizeof(cmd.emoji), get_string(data, "emoji"));
                cmd.size = get_int(data, "size", 0);
                g_router.on_display(&cmd);
            }
        }
    } else if (strcmp(type, "status") == 0) {
        msg_type = WS_MSG_STATUS;
        if (g_router.on_status) {
            /* v2.0 format: data is string */
            const char *data_str = get_string(root, "data");
            ws_status_cmd_t cmd = {0};
            copy_string(cmd.data, sizeof(cmd.data), data_str);
            g_router.on_status(&cmd);
        }
    } else if (strcmp(type, "asr_result") == 0) {
        msg_type = WS_MSG_ASR_RESULT;
        if (g_router.on_asr_result) {
            const char *data_str = get_string(root, "data");
            ws_asr_result_cmd_t cmd = {0};
            copy_string(cmd.text, sizeof(cmd.text), data_str);
            g_router.on_asr_result(&cmd);
        }
    } else if (strcmp(type, "bot_reply") == 0) {
        msg_type = WS_MSG_BOT_REPLY;
        if (g_router.on_bot_reply) {
            const char *data_str = get_string(root, "data");
            ws_bot_reply_cmd_t cmd = {0};
            copy_string(cmd.text, sizeof(cmd.text), data_str);
            g_router.on_bot_reply(&cmd);
        }
    } else if (strcmp(type, "tts_end") == 0) {
        msg_type = WS_MSG_TTS_END;
        if (g_router.on_tts_end) {
            g_router.on_tts_end();
        }
    } else if (strcmp(type, "error") == 0) {
        msg_type = WS_MSG_ERROR_MSG;
        if (g_router.on_error) {
            ws_error_cmd_t cmd = {0};
            cmd.code = get_int(root, "code", 1);
            const char *data_str = get_string(root, "data");
            copy_string(cmd.message, sizeof(cmd.message), data_str);
            g_router.on_error(&cmd);
        }
    } else if (strcmp(type, "capture") == 0) {
        msg_type = WS_MSG_CAPTURE;
        if (g_router.on_capture) {
            cJSON *data = cJSON_GetObjectItem(root, "data");
            ws_capture_cmd_t cmd = {0};
            fill_camera_cmd(data, "single", &cmd);
            if (data) {
                cmd.fps = get_int(data, "fps", 5);
                cmd.quality = get_int(data, "quality", 80);
            } else {
                cmd.fps = 5;
                cmd.quality = 80;
            }
            if (cmd.action[0] == '\0') {
                copy_string(cmd.action, sizeof(cmd.action), "single");
            }
            g_router.on_capture(&cmd);
        }
    } else if (strcmp(type, "ctrl.camera.video_config") == 0) {
        msg_type = WS_MSG_CTRL_CAMERA_VIDEO_CONFIG;
        if (g_router.on_capture) {
            cJSON *data = cJSON_GetObjectItem(root, "data");
            ws_capture_cmd_t cmd = {0};
            fill_camera_cmd(data, "config", &cmd);
            g_router.on_capture(&cmd);
        }
    } else if (strcmp(type, "ctrl.camera.capture_image") == 0) {
        msg_type = WS_MSG_CTRL_CAMERA_CAPTURE_IMAGE;
        if (g_router.on_capture) {
            cJSON *data = cJSON_GetObjectItem(root, "data");
            ws_capture_cmd_t cmd = {0};
            fill_camera_cmd(data, "single", &cmd);
            g_router.on_capture(&cmd);
        }
    } else if (strcmp(type, "ctrl.camera.start_video") == 0) {
        msg_type = WS_MSG_CTRL_CAMERA_START_VIDEO;
        if (g_router.on_capture) {
            cJSON *data = cJSON_GetObjectItem(root, "data");
            ws_capture_cmd_t cmd = {0};
            fill_camera_cmd(data, "start", &cmd);
            g_router.on_capture(&cmd);
        }
    } else if (strcmp(type, "ctrl.camera.stop_video") == 0) {
        msg_type = WS_MSG_CTRL_CAMERA_STOP_VIDEO;
        if (g_router.on_capture) {
            cJSON *data = cJSON_GetObjectItem(root, "data");
            ws_capture_cmd_t cmd = {0};
            fill_camera_cmd(data, "stop", &cmd);
            g_router.on_capture(&cmd);
        }
    } else if (strcmp(type, "reboot") == 0) {
        msg_type = WS_MSG_REBOOT;
        if (g_router.on_reboot) {
            g_router.on_reboot();
        }
    }
    /* Media stream types - recognized but no handler */
    else if (strcmp(type, "audio") == 0) {
        msg_type = WS_MSG_AUDIO;
    } else if (strcmp(type, "audio_end") == 0) {
        msg_type = WS_MSG_AUDIO_END;
    } else if (strcmp(type, "video") == 0) {
        msg_type = WS_MSG_VIDEO;
    } else if (strcmp(type, "sensor") == 0) {
        msg_type = WS_MSG_SENSOR;
    } else if (strcmp(type, "ping") == 0) {
        msg_type = WS_MSG_PING;
    } else if (strcmp(type, "pong") == 0) {
        msg_type = WS_MSG_PONG;
    } else if (strcmp(type, "connected") == 0) {
        msg_type = WS_MSG_CONNECTED;
    }

    cJSON_Delete(root);
    return msg_type;
}

/* ------------------------------------------------------------------ */
/* Public: Parse servo command (v2.1 format)                         */
/* ------------------------------------------------------------------ */

int ws_parse_servo(const char *json_str, ws_servo_cmd_t *out_cmd) {
    if (!json_str || !out_cmd) {
        return -1;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        return -1;
    }

    /* v2.1 format: data.id, data.angle, data.time */
    memset(out_cmd, 0, sizeof(*out_cmd));
    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (data && cJSON_IsObject(data)) {
        copy_string(out_cmd->id, sizeof(out_cmd->id), get_string(data, "id"));
        int angle1 = get_int(data, "angle", INT_MIN);
        int angle2 = get_int(data, "Angle", INT_MIN);
        out_cmd->angle = (angle1 != INT_MIN) ? angle1 : angle2;
        if (out_cmd->angle == INT_MIN)
            out_cmd->angle = 90;
        out_cmd->time_ms = get_int(data, "time", 100);
    } else {
        /* Default values */
        strncpy(out_cmd->id, "x", sizeof(out_cmd->id) - 1);
        out_cmd->angle = 90;
        out_cmd->time_ms = 100;
    }

    cJSON_Delete(root);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public: Parse display command                                      */
/* ------------------------------------------------------------------ */

int ws_parse_display(const char *json_str, ws_display_cmd_t *out_cmd) {
    if (!json_str || !out_cmd) {
        return -1;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        return -1;
    }

    memset(out_cmd, 0, sizeof(*out_cmd));

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (data && cJSON_IsObject(data)) {
        copy_string(out_cmd->text, sizeof(out_cmd->text), get_string(data, "text"));
        copy_string(out_cmd->emoji, sizeof(out_cmd->emoji), get_string(data, "emoji"));
        out_cmd->size = get_int(data, "size", 0);
    }

    cJSON_Delete(root);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public: Parse status command (v2.0 format)                         */
/* ------------------------------------------------------------------ */

int ws_parse_status(const char *json_str, ws_status_cmd_t *out_cmd) {
    if (!json_str || !out_cmd) {
        return -1;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        return -1;
    }

    memset(out_cmd, 0, sizeof(*out_cmd));

    /* v2.0 format: data is string */
    const char *data_str = get_string(root, "data");
    copy_string(out_cmd->data, sizeof(out_cmd->data), data_str);

    cJSON_Delete(root);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public: Parse ASR result (v2.0 format)                            */
/* ------------------------------------------------------------------ */

int ws_parse_asr_result(const char *json_str, ws_asr_result_cmd_t *out_cmd) {
    if (!json_str || !out_cmd) {
        return -1;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        return -1;
    }

    memset(out_cmd, 0, sizeof(*out_cmd));

    const char *data_str = get_string(root, "data");
    copy_string(out_cmd->text, sizeof(out_cmd->text), data_str);

    cJSON_Delete(root);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public: Parse bot reply (v2.0 format)                             */
/* ------------------------------------------------------------------ */

int ws_parse_bot_reply(const char *json_str, ws_bot_reply_cmd_t *out_cmd) {
    if (!json_str || !out_cmd) {
        return -1;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        return -1;
    }

    memset(out_cmd, 0, sizeof(*out_cmd));

    const char *data_str = get_string(root, "data");
    copy_string(out_cmd->text, sizeof(out_cmd->text), data_str);

    cJSON_Delete(root);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public: Parse error message (v2.0 format)                         */
/* ------------------------------------------------------------------ */

int ws_parse_error(const char *json_str, ws_error_cmd_t *out_cmd) {
    if (!json_str || !out_cmd) {
        return -1;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        return -1;
    }

    memset(out_cmd, 0, sizeof(*out_cmd));

    out_cmd->code = get_int(root, "code", 1);
    const char *data_str = get_string(root, "data");
    copy_string(out_cmd->message, sizeof(out_cmd->message), data_str);

    cJSON_Delete(root);
    return 0;
}
