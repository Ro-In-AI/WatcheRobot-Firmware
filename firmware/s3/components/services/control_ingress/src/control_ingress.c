#include "control_ingress.h"

#include "behavior_state_service.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hal_servo.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define TAG "CTRL_INGRESS"

#define CONTROL_STATE_QUEUE_DEPTH 16
#define CONTROL_STATE_TASK_STACK 6144
#define CONTROL_STATE_TASK_PRIORITY 5

typedef enum {
    CONTROL_STATE_MSG_AI_STATUS = 0,
    CONTROL_STATE_MSG_STATE_SET,
    CONTROL_STATE_MSG_STATE_TEXT,
} control_state_msg_type_t;

typedef struct {
    control_state_msg_type_t type;
    union {
        control_ai_status_request_t ai_status;
        control_state_set_request_t state_set;
        control_state_text_request_t state_text;
    } data;
} control_state_msg_t;

static QueueHandle_t s_state_queue = NULL;
static TaskHandle_t s_state_task = NULL;

static bool control_contains_nocase(const char *haystack, const char *needle) {
    size_t needle_len;

    if (haystack == NULL || needle == NULL || needle[0] == '\0') {
        return false;
    }

    needle_len = strlen(needle);
    while (*haystack != '\0') {
        if (strncasecmp(haystack, needle, needle_len) == 0) {
            return true;
        }
        haystack++;
    }

    return false;
}

static void control_normalize_resource_name(const char *raw, char *out, size_t out_size) {
    const char *base;
    const char *ext;
    size_t len;
    size_t i;

    if (out == NULL || out_size == 0) {
        return;
    }

    out[0] = '\0';
    if (raw == NULL || raw[0] == '\0') {
        return;
    }

    base = raw;
    for (i = 0; raw[i] != '\0'; ++i) {
        if (raw[i] == '/' || raw[i] == '\\') {
            base = &raw[i + 1];
        }
    }

    ext = strrchr(base, '.');
    len = (ext != NULL && ext > base) ? (size_t)(ext - base) : strlen(base);
    if (len >= out_size) {
        len = out_size - 1;
    }

    for (i = 0; i < len; ++i) {
        out[i] = (char)tolower((unsigned char)base[i]);
    }
    out[len] = '\0';
}

static bool control_append_state_candidate(const char **candidates,
                                           size_t *count,
                                           size_t max_count,
                                           const char *candidate) {
    size_t i;

    if (candidates == NULL || count == NULL || candidate == NULL || candidate[0] == '\0') {
        return false;
    }

    for (i = 0; i < *count; ++i) {
        if (strcasecmp(candidates[i], candidate) == 0) {
            return false;
        }
    }

    if (*count >= max_count) {
        return false;
    }

    candidates[*count] = candidate;
    (*count)++;
    return true;
}

static const char *control_ai_status_to_fallback(const char *status, const char *message) {
    if (control_contains_nocase(status, "observing") || control_contains_nocase(message, "observing")) {
        return "custom3";
    }
    if (control_contains_nocase(status, "listening") || control_contains_nocase(message, "listening")) {
        return "listening";
    }
    if (control_contains_nocase(status, "thinking") || control_contains_nocase(message, "thinking")) {
        return "thinking";
    }
    if (control_contains_nocase(status, "processing") || control_contains_nocase(status, "analyzing") ||
        control_contains_nocase(message, "processing") || control_contains_nocase(message, "analyzing")) {
        return "processing";
    }
    if (control_contains_nocase(status, "speaking") || control_contains_nocase(message, "speaking")) {
        return "speaking";
    }
    if (control_contains_nocase(status, "idle") || control_contains_nocase(message, "idle")) {
        return "standby";
    }
    if (control_contains_nocase(status, "done") || control_contains_nocase(status, "completed") ||
        control_contains_nocase(message, "done") || control_contains_nocase(message, "completed")) {
        return "happy";
    }
    if (control_contains_nocase(status, "error") || control_contains_nocase(status, "fail") ||
        control_contains_nocase(message, "error") || control_contains_nocase(message, "fail")) {
        return "error";
    }

    return NULL;
}

static void control_apply_ai_status(const control_ai_status_request_t *req) {
    char action_state_id[sizeof(req->action_file)];
    char status_state_id[sizeof(req->status)];
    char fallback_state_id[sizeof(req->status)];
    char image_name[sizeof(req->image_name)];
    char sound_id[sizeof(req->sound_file)];
    const char *state_candidates[3] = {0};
    const char *action_candidates[3] = {0};
    const char *selected_action_id = NULL;
    const char *text;
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    size_t state_candidate_count = 0;
    size_t action_candidate_count = 0;
    size_t i;

    control_normalize_resource_name(req->action_file, action_state_id, sizeof(action_state_id));
    control_normalize_resource_name(req->status, status_state_id, sizeof(status_state_id));
    control_normalize_resource_name(req->image_name, image_name, sizeof(image_name));
    control_normalize_resource_name(req->sound_file, sound_id, sizeof(sound_id));
    control_normalize_resource_name(control_ai_status_to_fallback(req->status, req->message),
                                    fallback_state_id,
                                    sizeof(fallback_state_id));

    text = req->message[0] != '\0' ? req->message : NULL;

    control_append_state_candidate(state_candidates, &state_candidate_count, 3, action_state_id);
    control_append_state_candidate(state_candidates, &state_candidate_count, 3, status_state_id);
    control_append_state_candidate(state_candidates, &state_candidate_count, 3, fallback_state_id);
    control_append_state_candidate(action_candidates, &action_candidate_count, 3, action_state_id);
    control_append_state_candidate(action_candidates, &action_candidate_count, 3, status_state_id);
    control_append_state_candidate(action_candidates, &action_candidate_count, 3, fallback_state_id);

    for (i = 0; i < action_candidate_count; ++i) {
        if (behavior_state_has_action(action_candidates[i])) {
            selected_action_id = action_candidates[i];
            break;
        }
    }

    for (i = 0; i < state_candidate_count; ++i) {
        ret = behavior_state_set_with_resources_and_action(state_candidates[i],
                                                           text,
                                                           0,
                                                           image_name[0] != '\0' ? image_name : NULL,
                                                           sound_id[0] != '\0' ? sound_id : NULL,
                                                           selected_action_id);
        if (ret != ESP_ERR_NOT_FOUND) {
            break;
        }
    }

    if (ret == ESP_ERR_NOT_FOUND) {
        ret = behavior_state_set_with_resources_and_action("standby",
                                                           text,
                                                           0,
                                                           image_name[0] != '\0' ? image_name : NULL,
                                                           sound_id[0] != '\0' ? sound_id : NULL,
                                                           selected_action_id);
        if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG,
                     "No local match for AI status=%s action=%s fallback=%s image=%s",
                     req->status,
                     action_state_id[0] != '\0' ? action_state_id : "<none>",
                     fallback_state_id[0] != '\0' ? fallback_state_id : "<none>",
                     image_name[0] != '\0' ? image_name : "<none>");
        }
    }

    if (ret != ESP_OK && text != NULL) {
        ret = behavior_state_set_text(text, 0);
    }

    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "AI status apply failed: %s", esp_err_to_name(ret));
    }
}

static void control_state_task(void *arg) {
    control_state_msg_t msg;

    (void)arg;

    while (true) {
        if (xQueueReceive(s_state_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (msg.type) {
            case CONTROL_STATE_MSG_AI_STATUS:
                control_apply_ai_status(&msg.data.ai_status);
                break;

            case CONTROL_STATE_MSG_STATE_SET: {
                esp_err_t ret = behavior_state_set(msg.data.state_set.state_id);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "State set failed: state=%s err=%s", msg.data.state_set.state_id, esp_err_to_name(ret));
                }
                break;
            }

            case CONTROL_STATE_MSG_STATE_TEXT: {
                const char *text = msg.data.state_text.text[0] != '\0' ? msg.data.state_text.text : NULL;
                esp_err_t ret;

                if (msg.data.state_text.state_id[0] != '\0') {
                    ret = behavior_state_set_with_text(msg.data.state_text.state_id, text, msg.data.state_text.font_size);
                } else if (text != NULL) {
                    ret = behavior_state_set_text(text, msg.data.state_text.font_size);
                } else {
                    ret = ESP_ERR_INVALID_ARG;
                }

                if (ret != ESP_OK) {
                    ESP_LOGW(TAG,
                             "State text apply failed: state=%s err=%s",
                             msg.data.state_text.state_id[0] != '\0' ? msg.data.state_text.state_id : "<none>",
                             esp_err_to_name(ret));
                }
                break;
            }

            default:
                ESP_LOGW(TAG, "Unknown control state msg: %d", (int)msg.type);
                break;
        }
    }
}

static esp_err_t control_submit_state_msg(const control_state_msg_t *msg) {
    if (msg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(s_state_queue, msg, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t control_ingress_init(void) {
    if (s_state_queue != NULL && s_state_task != NULL) {
        return ESP_OK;
    }

    s_state_queue = xQueueCreate(CONTROL_STATE_QUEUE_DEPTH, sizeof(control_state_msg_t));
    if (s_state_queue == NULL) {
        ESP_LOGE(TAG, "state queue create failed");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(control_state_task,
                    "control_state",
                    CONTROL_STATE_TASK_STACK,
                    NULL,
                    CONTROL_STATE_TASK_PRIORITY,
                    &s_state_task) != pdPASS) {
        vQueueDelete(s_state_queue);
        s_state_queue = NULL;
        s_state_task = NULL;
        ESP_LOGE(TAG, "state task create failed");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t control_ingress_submit_servo(const control_servo_request_t *req) {
    if (req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (req->duration_ms <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!req->has_x && !req->has_y) {
        return ESP_ERR_INVALID_ARG;
    }
    if (req->has_x && (req->x_deg < 0 || req->x_deg > 180)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (req->has_y && (req->y_deg < 0 || req->y_deg > 180)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (req->has_x && req->has_y) {
        return hal_servo_move_sync(req->x_deg, req->y_deg, req->duration_ms);
    }

    return hal_servo_move_smooth(req->has_x ? SERVO_AXIS_X : SERVO_AXIS_Y,
                                 req->has_x ? req->x_deg : req->y_deg,
                                 req->duration_ms);
}

esp_err_t control_ingress_submit_ai_status(const control_ai_status_request_t *req) {
    control_state_msg_t msg = {0};

    if (req == NULL || req->status[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    msg.type = CONTROL_STATE_MSG_AI_STATUS;
    msg.data.ai_status = *req;
    return control_submit_state_msg(&msg);
}

esp_err_t control_ingress_submit_state_set(const control_state_set_request_t *req) {
    control_state_msg_t msg = {0};

    if (req == NULL || req->state_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    msg.type = CONTROL_STATE_MSG_STATE_SET;
    msg.data.state_set = *req;
    return control_submit_state_msg(&msg);
}

esp_err_t control_ingress_submit_state_text(const control_state_text_request_t *req) {
    control_state_msg_t msg = {0};

    if (req == NULL || (req->state_id[0] == '\0' && req->text[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }

    msg.type = CONTROL_STATE_MSG_STATE_TEXT;
    msg.data.state_text = *req;
    return control_submit_state_msg(&msg);
}
