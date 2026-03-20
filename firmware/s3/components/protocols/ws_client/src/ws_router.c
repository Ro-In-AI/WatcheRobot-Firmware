/**
 * @file ws_router.c
 * @brief WebSocket message router implementation (protocol v0.1.1)
 */

#include "ws_router.h"
#include "cJSON.h"
#include <string.h>

static ws_router_t g_router = {0};

static const char *get_string(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsString(item)) {
        return item->valuestring;
    }
    return NULL;
}

static cJSON *get_object(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsObject(item)) {
        return item;
    }
    return NULL;
}

static int get_int(cJSON *obj, const char *key, int default_val) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return default_val;
}

static bool get_float(cJSON *obj, const char *key, float *out_value) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsNumber(item) && out_value) {
        *out_value = (float)item->valuedouble;
        return true;
    }
    return false;
}

static void copy_string(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) {
        return;
    }

    if (src) {
        strncpy(dst, src, dst_size - 1);
        dst[dst_size - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

static void fill_sys_ack(cJSON *root, ws_sys_ack_t *ack) {
    cJSON *data = get_object(root, "data");

    memset(ack, 0, sizeof(*ack));
    ack->code = get_int(root, "code", 0);
    if (data) {
        copy_string(ack->type, sizeof(ack->type), get_string(data, "type"));
        if (ack->type[0] == '\0') {
            copy_string(ack->type, sizeof(ack->type), get_string(data, "ref_type"));
        }
        copy_string(ack->command_id, sizeof(ack->command_id), get_string(data, "command_id"));
        if (ack->command_id[0] == '\0') {
            copy_string(ack->command_id, sizeof(ack->command_id), get_string(data, "ref_id"));
        }
        copy_string(ack->message, sizeof(ack->message), get_string(data, "message"));
    }
}

static void fill_sys_nack(cJSON *root, ws_sys_nack_t *nack) {
    cJSON *data = get_object(root, "data");

    memset(nack, 0, sizeof(*nack));
    nack->code = get_int(root, "code", 1);
    if (data) {
        copy_string(nack->type, sizeof(nack->type), get_string(data, "type"));
        if (nack->type[0] == '\0') {
            copy_string(nack->type, sizeof(nack->type), get_string(data, "ref_type"));
        }
        copy_string(nack->command_id, sizeof(nack->command_id), get_string(data, "command_id"));
        if (nack->command_id[0] == '\0') {
            copy_string(nack->command_id, sizeof(nack->command_id), get_string(data, "ref_id"));
        }
        copy_string(nack->reason, sizeof(nack->reason), get_string(data, "reason"));
        if (nack->reason[0] == '\0') {
            copy_string(nack->reason, sizeof(nack->reason), get_string(data, "message"));
        }
    }
}

static void fill_servo_cmd(cJSON *root, ws_servo_cmd_t *cmd) {
    cJSON *data = get_object(root, "data");

    memset(cmd, 0, sizeof(*cmd));
    cmd->duration_ms = 100;
    if (!data) {
        return;
    }

    cmd->has_x = get_float(data, "x_deg", &cmd->x_deg);
    cmd->has_y = get_float(data, "y_deg", &cmd->y_deg);
    cmd->duration_ms = get_int(data, "duration_ms", 100);
}

static void fill_text_event(cJSON *root, ws_text_event_t *event) {
    cJSON *data = get_object(root, "data");

    memset(event, 0, sizeof(*event));
    if (data) {
        copy_string(event->text, sizeof(event->text), get_string(data, "text"));
    }
}

static void fill_ai_status(cJSON *root, ws_ai_status_t *event) {
    cJSON *data = get_object(root, "data");

    memset(event, 0, sizeof(*event));
    if (data) {
        copy_string(event->status, sizeof(event->status), get_string(data, "status"));
        copy_string(event->message, sizeof(event->message), get_string(data, "message"));
    }
}

static void fill_ai_thinking(cJSON *root, ws_ai_thinking_t *event) {
    cJSON *data = get_object(root, "data");

    memset(event, 0, sizeof(*event));
    if (data) {
        copy_string(event->kind, sizeof(event->kind), get_string(data, "kind"));
        copy_string(event->content, sizeof(event->content), get_string(data, "content"));
    }
}

static void fill_capture_cmd(cJSON *root, const char *action, ws_capture_cmd_t *cmd) {
    cJSON *data = get_object(root, "data");

    memset(cmd, 0, sizeof(*cmd));
    copy_string(cmd->action, sizeof(cmd->action), action);
    if (data) {
        copy_string(cmd->command_id, sizeof(cmd->command_id), get_string(data, "command_id"));
        cmd->width = get_int(data, "width", 0);
        cmd->height = get_int(data, "height", 0);
        cmd->fps = get_int(data, "fps", 0);
        cmd->quality = get_int(data, "quality", 0);
    }
}

static void fill_transfer_cmd(cJSON *root, const char *message_type, ws_transfer_cmd_t *cmd) {
    cJSON *data = get_object(root, "data");

    memset(cmd, 0, sizeof(*cmd));
    copy_string(cmd->message_type, sizeof(cmd->message_type), message_type);
    if (data) {
        copy_string(cmd->transfer_id, sizeof(cmd->transfer_id), get_string(data, "transfer_id"));
        copy_string(cmd->sha256, sizeof(cmd->sha256), get_string(data, "sha256"));
    }
}

void ws_router_init(ws_router_t *router) {
    if (router) {
        g_router = *router;
    } else {
        memset(&g_router, 0, sizeof(g_router));
    }
}

ws_msg_type_t ws_route_message(const char *json_str) {
    ws_msg_type_t msg_type = WS_MSG_UNKNOWN;
    cJSON *root;
    const char *type;

    if (!json_str) {
        return WS_MSG_UNKNOWN;
    }

    root = cJSON_Parse(json_str);
    if (!root) {
        return WS_MSG_UNKNOWN;
    }

    type = get_string(root, "type");
    if (!type) {
        cJSON_Delete(root);
        return WS_MSG_UNKNOWN;
    }

    if (strcmp(type, "sys.ack") == 0) {
        msg_type = WS_MSG_SYS_ACK;
        if (g_router.on_sys_ack) {
            ws_sys_ack_t ack;
            fill_sys_ack(root, &ack);
            g_router.on_sys_ack(&ack);
        }
    } else if (strcmp(type, "sys.nack") == 0) {
        msg_type = WS_MSG_SYS_NACK;
        if (g_router.on_sys_nack) {
            ws_sys_nack_t nack;
            fill_sys_nack(root, &nack);
            g_router.on_sys_nack(&nack);
        }
    } else if (strcmp(type, "sys.ping") == 0) {
        msg_type = WS_MSG_SYS_PING;
        if (g_router.on_sys_ping) {
            g_router.on_sys_ping();
        }
    } else if (strcmp(type, "sys.pong") == 0) {
        msg_type = WS_MSG_SYS_PONG;
        if (g_router.on_sys_pong) {
            g_router.on_sys_pong();
        }
    } else if (strcmp(type, "sys.session.resume") == 0) {
        msg_type = WS_MSG_SYS_SESSION_RESUME;
        if (g_router.on_session_resume) {
            g_router.on_session_resume();
        }
    } else if (strcmp(type, "ctrl.servo.angle") == 0) {
        msg_type = WS_MSG_CTRL_SERVO_ANGLE;
        if (g_router.on_servo) {
            ws_servo_cmd_t cmd;
            fill_servo_cmd(root, &cmd);
            g_router.on_servo(&cmd);
        }
    } else if (strcmp(type, "ctrl.camera.video_config") == 0) {
        msg_type = WS_MSG_CTRL_CAMERA_VIDEO_CONFIG;
        if (g_router.on_capture) {
            ws_capture_cmd_t cmd;
            fill_capture_cmd(root, "config", &cmd);
            g_router.on_capture(&cmd);
        }
    } else if (strcmp(type, "ctrl.camera.capture_image") == 0) {
        msg_type = WS_MSG_CTRL_CAMERA_CAPTURE_IMAGE;
        if (g_router.on_capture) {
            ws_capture_cmd_t cmd;
            fill_capture_cmd(root, "single", &cmd);
            g_router.on_capture(&cmd);
        }
    } else if (strcmp(type, "ctrl.camera.start_video") == 0) {
        msg_type = WS_MSG_CTRL_CAMERA_START_VIDEO;
        if (g_router.on_capture) {
            ws_capture_cmd_t cmd;
            fill_capture_cmd(root, "start", &cmd);
            g_router.on_capture(&cmd);
        }
    } else if (strcmp(type, "ctrl.camera.stop_video") == 0) {
        msg_type = WS_MSG_CTRL_CAMERA_STOP_VIDEO;
        if (g_router.on_capture) {
            ws_capture_cmd_t cmd;
            fill_capture_cmd(root, "stop", &cmd);
            g_router.on_capture(&cmd);
        }
    } else if (strcmp(type, "evt.asr.result") == 0) {
        msg_type = WS_MSG_EVT_ASR_RESULT;
        if (g_router.on_asr_result) {
            ws_text_event_t event;
            fill_text_event(root, &event);
            g_router.on_asr_result(&event);
        }
    } else if (strcmp(type, "evt.ai.status") == 0) {
        msg_type = WS_MSG_EVT_AI_STATUS;
        if (g_router.on_ai_status) {
            ws_ai_status_t event;
            fill_ai_status(root, &event);
            g_router.on_ai_status(&event);
        }
    } else if (strcmp(type, "evt.ai.thinking") == 0) {
        msg_type = WS_MSG_EVT_AI_THINKING;
        if (g_router.on_ai_thinking) {
            ws_ai_thinking_t event;
            fill_ai_thinking(root, &event);
            g_router.on_ai_thinking(&event);
        }
    } else if (strcmp(type, "evt.ai.reply") == 0) {
        msg_type = WS_MSG_EVT_AI_REPLY;
        if (g_router.on_ai_reply) {
            ws_text_event_t event;
            fill_text_event(root, &event);
            g_router.on_ai_reply(&event);
        }
    } else if (strcmp(type, "xfer.ota.handshake") == 0) {
        msg_type = WS_MSG_XFER_OTA_HANDSHAKE;
        if (g_router.on_transfer) {
            ws_transfer_cmd_t cmd;
            fill_transfer_cmd(root, type, &cmd);
            g_router.on_transfer(&cmd);
        }
    } else if (strcmp(type, "xfer.ota.checksum") == 0) {
        msg_type = WS_MSG_XFER_OTA_CHECKSUM;
        if (g_router.on_transfer) {
            ws_transfer_cmd_t cmd;
            fill_transfer_cmd(root, type, &cmd);
            g_router.on_transfer(&cmd);
        }
    }

    cJSON_Delete(root);
    return msg_type;
}
