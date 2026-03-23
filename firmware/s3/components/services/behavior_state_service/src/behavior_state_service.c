#include "behavior_state_service.h"

#include "anim_storage.h"
#include "cJSON.h"
#include "display_ui.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal_servo.h"
#include "sfx_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "BEHAVIOR_STATE"

#define BEHAVIOR_STATES_PATH "/spiffs/behavior/states.json"
#define BEHAVIOR_TASK_STACK 6144
#define BEHAVIOR_TASK_PRIORITY 5
#define BEHAVIOR_TICK_MS 10
#define BEHAVIOR_MAX_FILE_BYTES 16384
#define BEHAVIOR_VERSION_LEN 16
#define BEHAVIOR_STATE_ID_LEN 32
#define BEHAVIOR_TEXT_LEN 128
#define BEHAVIOR_SOUND_ID_LEN 32
#define BEHAVIOR_DEFAULT_ONESHOT_HOLD_MS 1200U

typedef struct {
    uint32_t at_ms;
    int x_deg;
    int y_deg;
    int duration_ms;
} behavior_motion_event_t;

typedef struct {
    uint32_t at_ms;
    char anim[BEHAVIOR_STATE_ID_LEN];
    char text[BEHAVIOR_TEXT_LEN];
    int font_size;
} behavior_expression_event_t;

typedef struct {
    uint32_t at_ms;
    char sound_id[BEHAVIOR_SOUND_ID_LEN];
} behavior_sound_event_t;

typedef struct {
    char id[BEHAVIOR_STATE_ID_LEN];
    bool loop;
    uint32_t timeline_end_ms;
    behavior_motion_event_t *motion;
    int motion_count;
    behavior_expression_event_t *expression;
    int expression_count;
    behavior_sound_event_t *sound;
    int sound_count;
} behavior_state_def_t;

typedef struct {
    char version[BEHAVIOR_VERSION_LEN];
    char default_state[BEHAVIOR_STATE_ID_LEN];
    behavior_state_def_t *states;
    int state_count;
} behavior_catalog_t;

typedef struct {
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    bool initialized;
    behavior_catalog_t catalog;
    behavior_state_def_t *current_state;
    char current_state_id[BEHAVIOR_STATE_ID_LEN];
    uint32_t state_started_ms;
    int next_motion_index;
    int next_expression_index;
    int next_sound_index;
    char text_override[BEHAVIOR_TEXT_LEN];
    int text_override_font_size;
    bool text_override_valid;
} behavior_context_t;

static behavior_context_t s_ctx = {0};

static void behavior_copy_string(char *dst, size_t dst_size, const char *src) {
    if (dst == NULL || dst_size == 0) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static bool behavior_lock(void) {
    if (s_ctx.lock == NULL) {
        return false;
    }

    return xSemaphoreTake(s_ctx.lock, portMAX_DELAY) == pdTRUE;
}

static void behavior_unlock(void) {
    if (s_ctx.lock != NULL) {
        xSemaphoreGive(s_ctx.lock);
    }
}

static uint32_t behavior_now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static int behavior_get_non_negative_int(cJSON *obj, const char *key, int default_value) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item != NULL && cJSON_IsNumber(item) && item->valuedouble >= 0) {
        return item->valueint;
    }
    return default_value;
}

static const char *behavior_get_string(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item != NULL && cJSON_IsString(item)) {
        return item->valuestring;
    }
    return NULL;
}

static void behavior_free_catalog(behavior_catalog_t *catalog) {
    int i;

    if (catalog == NULL) {
        return;
    }

    if (catalog->states != NULL) {
        for (i = 0; i < catalog->state_count; ++i) {
            free(catalog->states[i].motion);
            free(catalog->states[i].expression);
            free(catalog->states[i].sound);
        }
    }

    free(catalog->states);
    memset(catalog, 0, sizeof(*catalog));
}

static behavior_state_def_t *behavior_find_state_locked(const char *state_id) {
    int i;

    if (state_id == NULL || state_id[0] == '\0') {
        return NULL;
    }

    for (i = 0; i < s_ctx.catalog.state_count; ++i) {
        if (strcmp(s_ctx.catalog.states[i].id, state_id) == 0) {
            return &s_ctx.catalog.states[i];
        }
    }

    return NULL;
}

static char *behavior_read_text_file(const char *path, size_t max_bytes) {
    FILE *file = NULL;
    char *buffer = NULL;
    long file_size;
    size_t read_size;

    file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (file_size <= 0 || (size_t)file_size > max_bytes) {
        fclose(file);
        return NULL;
    }

    buffer = (char *)calloc((size_t)file_size + 1U, 1U);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }

    read_size = fread(buffer, 1, (size_t)file_size, file);
    fclose(file);
    buffer[read_size] = '\0';
    return buffer;
}

static esp_err_t behavior_parse_motion_events(cJSON *array,
                                              behavior_motion_event_t **out_events,
                                              int *out_count,
                                              uint32_t *out_timeline_end_ms) {
    int count;
    int index = 0;
    cJSON *item = NULL;
    behavior_motion_event_t *events = NULL;

    *out_events = NULL;
    *out_count = 0;
    if (array == NULL) {
        return ESP_OK;
    }
    if (!cJSON_IsArray(array)) {
        return ESP_ERR_INVALID_ARG;
    }

    count = cJSON_GetArraySize(array);
    if (count <= 0) {
        return ESP_OK;
    }

    events = (behavior_motion_event_t *)calloc((size_t)count, sizeof(behavior_motion_event_t));
    if (events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_ArrayForEach(item, array) {
        int x_deg;
        int y_deg;
        int duration_ms;
        uint32_t at_ms;

        if (!cJSON_IsObject(item)) {
            free(events);
            return ESP_ERR_INVALID_ARG;
        }

        x_deg = behavior_get_non_negative_int(item, "x_deg", -1);
        y_deg = behavior_get_non_negative_int(item, "y_deg", -1);
        duration_ms = behavior_get_non_negative_int(item, "duration_ms", 0);
        at_ms = (uint32_t)behavior_get_non_negative_int(item, "at_ms", 0);
        if (x_deg < 0 || x_deg > 180 || y_deg < 0 || y_deg > 180) {
            free(events);
            return ESP_ERR_INVALID_ARG;
        }

        events[index].at_ms = at_ms;
        events[index].x_deg = x_deg;
        events[index].y_deg = y_deg;
        events[index].duration_ms = duration_ms;
        if (at_ms + (uint32_t)duration_ms > *out_timeline_end_ms) {
            *out_timeline_end_ms = at_ms + (uint32_t)duration_ms;
        }
        index++;
    }

    *out_events = events;
    *out_count = count;
    return ESP_OK;
}

static esp_err_t behavior_parse_expression_events(cJSON *array,
                                                  behavior_expression_event_t **out_events,
                                                  int *out_count,
                                                  uint32_t *out_timeline_end_ms) {
    int count;
    int index = 0;
    cJSON *item = NULL;
    behavior_expression_event_t *events = NULL;

    *out_events = NULL;
    *out_count = 0;
    if (array == NULL) {
        return ESP_OK;
    }
    if (!cJSON_IsArray(array)) {
        return ESP_ERR_INVALID_ARG;
    }

    count = cJSON_GetArraySize(array);
    if (count <= 0) {
        return ESP_OK;
    }

    events = (behavior_expression_event_t *)calloc((size_t)count, sizeof(behavior_expression_event_t));
    if (events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_ArrayForEach(item, array) {
        const char *anim;
        const char *text;
        uint32_t at_ms;

        if (!cJSON_IsObject(item)) {
            free(events);
            return ESP_ERR_INVALID_ARG;
        }

        anim = behavior_get_string(item, "anim");
        text = behavior_get_string(item, "text");
        at_ms = (uint32_t)behavior_get_non_negative_int(item, "at_ms", 0);

        if ((anim == NULL || anim[0] == '\0') && (text == NULL || text[0] == '\0')) {
            free(events);
            return ESP_ERR_INVALID_ARG;
        }
        if (anim != NULL && anim[0] != '\0' && display_emoji_from_string(anim) == EMOJI_UNKNOWN) {
            free(events);
            return ESP_ERR_INVALID_ARG;
        }

        events[index].at_ms = at_ms;
        events[index].font_size = behavior_get_non_negative_int(item, "font_size", 0);
        behavior_copy_string(events[index].anim, sizeof(events[index].anim), anim);
        behavior_copy_string(events[index].text, sizeof(events[index].text), text);
        if (at_ms > *out_timeline_end_ms) {
            *out_timeline_end_ms = at_ms;
        }
        index++;
    }

    *out_events = events;
    *out_count = count;
    return ESP_OK;
}

static esp_err_t behavior_parse_sound_events(cJSON *array,
                                             behavior_sound_event_t **out_events,
                                             int *out_count,
                                             uint32_t *out_timeline_end_ms) {
    int count;
    int index = 0;
    cJSON *item = NULL;
    behavior_sound_event_t *events = NULL;

    *out_events = NULL;
    *out_count = 0;
    if (array == NULL) {
        return ESP_OK;
    }
    if (!cJSON_IsArray(array)) {
        return ESP_ERR_INVALID_ARG;
    }

    count = cJSON_GetArraySize(array);
    if (count <= 0) {
        return ESP_OK;
    }

    events = (behavior_sound_event_t *)calloc((size_t)count, sizeof(behavior_sound_event_t));
    if (events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_ArrayForEach(item, array) {
        const char *sound_id;
        uint32_t at_ms;

        if (!cJSON_IsObject(item)) {
            free(events);
            return ESP_ERR_INVALID_ARG;
        }

        sound_id = behavior_get_string(item, "sound_id");
        at_ms = (uint32_t)behavior_get_non_negative_int(item, "at_ms", 0);
        if (sound_id == NULL || sound_id[0] == '\0') {
            free(events);
            return ESP_ERR_INVALID_ARG;
        }

        events[index].at_ms = at_ms;
        behavior_copy_string(events[index].sound_id, sizeof(events[index].sound_id), sound_id);
        if (at_ms > *out_timeline_end_ms) {
            *out_timeline_end_ms = at_ms;
        }
        index++;
    }

    *out_events = events;
    *out_count = count;
    return ESP_OK;
}

static esp_err_t behavior_parse_state_def(const char *state_id, cJSON *obj, behavior_state_def_t *out_state) {
    cJSON *loop_item = NULL;
    uint32_t timeline_end_ms = 0;
    esp_err_t ret;

    if (state_id == NULL || state_id[0] == '\0' || obj == NULL || out_state == NULL || !cJSON_IsObject(obj)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_state, 0, sizeof(*out_state));
    behavior_copy_string(out_state->id, sizeof(out_state->id), state_id);

    loop_item = cJSON_GetObjectItem(obj, "loop");
    out_state->loop = (loop_item != NULL && cJSON_IsBool(loop_item) && cJSON_IsTrue(loop_item));

    ret = behavior_parse_motion_events(cJSON_GetObjectItem(obj, "motion"),
                                       &out_state->motion,
                                       &out_state->motion_count,
                                       &timeline_end_ms);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = behavior_parse_expression_events(cJSON_GetObjectItem(obj, "expression"),
                                           &out_state->expression,
                                           &out_state->expression_count,
                                           &timeline_end_ms);
    if (ret != ESP_OK) {
        free(out_state->motion);
        memset(out_state, 0, sizeof(*out_state));
        return ret;
    }

    ret = behavior_parse_sound_events(cJSON_GetObjectItem(obj, "sound"),
                                      &out_state->sound,
                                      &out_state->sound_count,
                                      &timeline_end_ms);
    if (ret != ESP_OK) {
        free(out_state->motion);
        free(out_state->expression);
        memset(out_state, 0, sizeof(*out_state));
        return ret;
    }

    out_state->timeline_end_ms = timeline_end_ms;
    return ESP_OK;
}

static esp_err_t behavior_load_catalog_from_file(behavior_catalog_t *out_catalog) {
    char *json = NULL;
    cJSON *root = NULL;
    cJSON *states_obj = NULL;
    cJSON *state_item = NULL;
    behavior_catalog_t catalog = {0};
    int expected_count = 0;
    int state_index = 0;

    json = behavior_read_text_file(BEHAVIOR_STATES_PATH, BEHAVIOR_MAX_FILE_BYTES);
    if (json == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    root = cJSON_Parse(json);
    free(json);
    if (root == NULL) {
        return ESP_FAIL;
    }

    behavior_copy_string(catalog.version,
                         sizeof(catalog.version),
                         behavior_get_string(root, "version") != NULL ? behavior_get_string(root, "version") : "1.0");
    behavior_copy_string(catalog.default_state,
                         sizeof(catalog.default_state),
                         behavior_get_string(root, "default_state") != NULL ? behavior_get_string(root, "default_state")
                                                                            : "standby");

    states_obj = cJSON_GetObjectItem(root, "states");
    if (states_obj == NULL || !cJSON_IsObject(states_obj)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON_ArrayForEach(state_item, states_obj) {
        if (state_item->string != NULL) {
            expected_count++;
        }
    }

    if (expected_count > 0) {
        catalog.states = (behavior_state_def_t *)calloc((size_t)expected_count, sizeof(behavior_state_def_t));
        if (catalog.states == NULL) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
    }

    cJSON_ArrayForEach(state_item, states_obj) {
        esp_err_t ret;

        if (state_item->string == NULL) {
            continue;
        }

        ret = behavior_parse_state_def(state_item->string, state_item, &catalog.states[state_index]);
        if (ret != ESP_OK) {
            behavior_free_catalog(&catalog);
            cJSON_Delete(root);
            return ret;
        }
        state_index++;
    }

    catalog.state_count = state_index;
    cJSON_Delete(root);
    *out_catalog = catalog;
    return ESP_OK;
}

static void behavior_reset_runtime_locked(void) {
    s_ctx.current_state = NULL;
    behavior_copy_string(s_ctx.current_state_id, sizeof(s_ctx.current_state_id), s_ctx.catalog.default_state);
    s_ctx.state_started_ms = behavior_now_ms();
    s_ctx.next_motion_index = 0;
    s_ctx.next_expression_index = 0;
    s_ctx.next_sound_index = 0;
    s_ctx.text_override[0] = '\0';
    s_ctx.text_override_font_size = 0;
    s_ctx.text_override_valid = false;
}

static void behavior_dispatch_motion_locked(const behavior_motion_event_t *event) {
    if (event == NULL) {
        return;
    }

    if (hal_servo_move_sync(event->x_deg, event->y_deg, event->duration_ms) != ESP_OK) {
        ESP_LOGW(TAG, "Servo motion failed: x=%d y=%d duration=%d", event->x_deg, event->y_deg, event->duration_ms);
    }
}

static void behavior_dispatch_expression_locked(const behavior_expression_event_t *event) {
    const char *text = NULL;
    int font_size = 0;

    if (event == NULL) {
        return;
    }

    if (s_ctx.text_override_valid) {
        text = s_ctx.text_override;
        font_size = s_ctx.text_override_font_size;
    } else if (event->text[0] != '\0') {
        text = event->text;
        font_size = event->font_size;
    }

    if (display_update(text, event->anim[0] != '\0' ? event->anim : NULL, font_size, NULL) != 0) {
        ESP_LOGW(TAG, "Display update failed for state '%s'", s_ctx.current_state_id);
    }
}

static void behavior_dispatch_sound_locked(const behavior_sound_event_t *event) {
    esp_err_t ret;

    if (event == NULL) {
        return;
    }

    ret = sfx_service_play(event->sound_id);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Skip sound '%s': audio_busy_tts", event->sound_id);
    } else if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to play sound '%s': %s", event->sound_id, esp_err_to_name(ret));
    }
}

static void behavior_dispatch_due_events_locked(uint32_t now_ms) {
    uint32_t elapsed_ms;

    if (s_ctx.current_state == NULL) {
        return;
    }

    elapsed_ms = now_ms - s_ctx.state_started_ms;

    while (s_ctx.next_motion_index < s_ctx.current_state->motion_count &&
           s_ctx.current_state->motion[s_ctx.next_motion_index].at_ms <= elapsed_ms) {
        behavior_dispatch_motion_locked(&s_ctx.current_state->motion[s_ctx.next_motion_index]);
        s_ctx.next_motion_index++;
    }

    while (s_ctx.next_expression_index < s_ctx.current_state->expression_count &&
           s_ctx.current_state->expression[s_ctx.next_expression_index].at_ms <= elapsed_ms) {
        behavior_dispatch_expression_locked(&s_ctx.current_state->expression[s_ctx.next_expression_index]);
        s_ctx.next_expression_index++;
    }

    while (s_ctx.next_sound_index < s_ctx.current_state->sound_count &&
           s_ctx.current_state->sound[s_ctx.next_sound_index].at_ms <= elapsed_ms) {
        behavior_dispatch_sound_locked(&s_ctx.current_state->sound[s_ctx.next_sound_index]);
        s_ctx.next_sound_index++;
    }

    if (s_ctx.current_state->expression_count == 0 && s_ctx.text_override_valid) {
        display_update(s_ctx.text_override, NULL, s_ctx.text_override_font_size, NULL);
    }
}

static bool behavior_all_events_dispatched_locked(void) {
    return s_ctx.current_state != NULL && s_ctx.next_motion_index >= s_ctx.current_state->motion_count &&
           s_ctx.next_expression_index >= s_ctx.current_state->expression_count &&
           s_ctx.next_sound_index >= s_ctx.current_state->sound_count;
}

static esp_err_t behavior_schedule_state_locked(const char *state_id, const char *text, int font_size) {
    behavior_state_def_t *state_def = behavior_find_state_locked(state_id);
    uint32_t now_ms = behavior_now_ms();

    if (state_def == NULL) {
        if (strcmp(state_id, s_ctx.catalog.default_state) != 0 && strcmp(state_id, "standby") != 0) {
            return ESP_ERR_NOT_FOUND;
        }

        s_ctx.current_state = NULL;
        behavior_copy_string(s_ctx.current_state_id, sizeof(s_ctx.current_state_id), s_ctx.catalog.default_state);
        s_ctx.state_started_ms = now_ms;
        s_ctx.next_motion_index = 0;
        s_ctx.next_expression_index = 0;
        s_ctx.next_sound_index = 0;
        s_ctx.text_override_valid = (text != NULL);
        behavior_copy_string(s_ctx.text_override, sizeof(s_ctx.text_override), text);
        s_ctx.text_override_font_size = font_size;
        if (display_update(text, s_ctx.catalog.default_state, font_size, NULL) != 0) {
            ESP_LOGW(TAG, "Fallback standby display update failed");
        }
        return ESP_OK;
    }

    s_ctx.current_state = state_def;
    behavior_copy_string(s_ctx.current_state_id, sizeof(s_ctx.current_state_id), state_def->id);
    s_ctx.state_started_ms = now_ms;
    s_ctx.next_motion_index = 0;
    s_ctx.next_expression_index = 0;
    s_ctx.next_sound_index = 0;
    s_ctx.text_override_valid = (text != NULL);
    behavior_copy_string(s_ctx.text_override, sizeof(s_ctx.text_override), text);
    s_ctx.text_override_font_size = font_size;
    behavior_dispatch_due_events_locked(now_ms);
    return ESP_OK;
}

static void behavior_task(void *arg) {
    char fallback_state[BEHAVIOR_STATE_ID_LEN];

    (void)arg;

    while (true) {
        bool should_fallback = false;

        fallback_state[0] = '\0';
        if (behavior_lock()) {
            uint32_t now_ms = behavior_now_ms();

            if (s_ctx.current_state != NULL) {
                uint32_t elapsed_ms;
                uint32_t done_at_ms;

                behavior_dispatch_due_events_locked(now_ms);
                elapsed_ms = now_ms - s_ctx.state_started_ms;

                if (s_ctx.current_state->loop) {
                    if (s_ctx.current_state->timeline_end_ms > 0 && behavior_all_events_dispatched_locked() &&
                        elapsed_ms >= s_ctx.current_state->timeline_end_ms) {
                        s_ctx.state_started_ms = now_ms;
                        s_ctx.next_motion_index = 0;
                        s_ctx.next_expression_index = 0;
                        s_ctx.next_sound_index = 0;
                        behavior_dispatch_due_events_locked(now_ms);
                    }
                } else {
                    done_at_ms = s_ctx.current_state->timeline_end_ms > 0 ? s_ctx.current_state->timeline_end_ms
                                                                          : BEHAVIOR_DEFAULT_ONESHOT_HOLD_MS;
                    if (behavior_all_events_dispatched_locked() && elapsed_ms >= done_at_ms) {
                        behavior_copy_string(fallback_state, sizeof(fallback_state), s_ctx.catalog.default_state);
                        s_ctx.current_state = NULL;
                        s_ctx.next_motion_index = 0;
                        s_ctx.next_expression_index = 0;
                        s_ctx.next_sound_index = 0;
                        s_ctx.text_override[0] = '\0';
                        s_ctx.text_override_font_size = 0;
                        s_ctx.text_override_valid = false;
                        should_fallback = true;
                    }
                }
            }

            behavior_unlock();
        }

        if (should_fallback && fallback_state[0] != '\0') {
            behavior_state_set(fallback_state);
        }

        vTaskDelay(pdMS_TO_TICKS(BEHAVIOR_TICK_MS));
    }
}

esp_err_t behavior_state_init(void) {
    BaseType_t task_result;
    esp_err_t load_ret;

    if (s_ctx.initialized) {
        return ESP_OK;
    }

    if (anim_catalog_init() != 0) {
        ESP_LOGW(TAG, "Animation catalog init failed during behavior init");
    }
    if (sfx_service_init() != ESP_OK) {
        ESP_LOGW(TAG, "SFX service init failed during behavior init");
    }

    s_ctx.lock = xSemaphoreCreateMutex();
    if (s_ctx.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    task_result = xTaskCreate(behavior_task, "behavior_state", BEHAVIOR_TASK_STACK, NULL, BEHAVIOR_TASK_PRIORITY, &s_ctx.task);
    if (task_result != pdPASS) {
        vSemaphoreDelete(s_ctx.lock);
        memset(&s_ctx, 0, sizeof(s_ctx));
        return ESP_ERR_NO_MEM;
    }

    behavior_copy_string(s_ctx.catalog.version, sizeof(s_ctx.catalog.version), "1.0");
    behavior_copy_string(s_ctx.catalog.default_state, sizeof(s_ctx.catalog.default_state), "standby");
    behavior_reset_runtime_locked();
    s_ctx.initialized = true;

    load_ret = behavior_state_load();
    if (load_ret != ESP_OK && load_ret != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "Behavior state load failed: %s", esp_err_to_name(load_ret));
    }

    return ESP_OK;
}

esp_err_t behavior_state_load(void) {
    behavior_catalog_t catalog = {0};
    esp_err_t ret;

    if (behavior_state_init() != ESP_OK) {
        return ESP_FAIL;
    }

    if (anim_catalog_init() != 0) {
        ESP_LOGW(TAG, "Animation catalog init failed during behavior load");
    }
    sfx_service_reload();

    ret = behavior_load_catalog_from_file(&catalog);
    if (ret != ESP_OK) {
        if (behavior_lock()) {
            behavior_free_catalog(&s_ctx.catalog);
            behavior_copy_string(s_ctx.catalog.version, sizeof(s_ctx.catalog.version), "1.0");
            behavior_copy_string(s_ctx.catalog.default_state, sizeof(s_ctx.catalog.default_state), "standby");
            behavior_reset_runtime_locked();
            behavior_unlock();
        }
        return ret;
    }

    if (behavior_lock()) {
        behavior_free_catalog(&s_ctx.catalog);
        s_ctx.catalog = catalog;
        behavior_reset_runtime_locked();
        behavior_unlock();
    }

    ESP_LOGI(TAG,
             "Loaded behavior states v%s: count=%d default=%s",
             s_ctx.catalog.version,
             s_ctx.catalog.state_count,
             s_ctx.catalog.default_state);
    return ESP_OK;
}

esp_err_t behavior_state_set(const char *state_id) {
    return behavior_state_set_with_text(state_id, NULL, 0);
}

esp_err_t behavior_state_set_with_text(const char *state_id, const char *text, int font_size) {
    esp_err_t ret;

    if (state_id == NULL || state_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (behavior_state_init() != ESP_OK) {
        return ESP_FAIL;
    }

    if (!behavior_lock()) {
        return ESP_FAIL;
    }

    ret = behavior_schedule_state_locked(state_id, text, font_size);
    behavior_unlock();
    return ret;
}

esp_err_t behavior_state_set_text(const char *text, int font_size) {
    if (behavior_state_init() != ESP_OK) {
        return ESP_FAIL;
    }

    if (!behavior_lock()) {
        return ESP_FAIL;
    }

    s_ctx.text_override_valid = (text != NULL);
    behavior_copy_string(s_ctx.text_override, sizeof(s_ctx.text_override), text);
    s_ctx.text_override_font_size = font_size;
    behavior_unlock();

    if (display_update(text, NULL, font_size, NULL) != 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

const char *behavior_state_get_current(void) {
    if (!s_ctx.initialized) {
        return "standby";
    }
    return s_ctx.current_state_id[0] != '\0' ? s_ctx.current_state_id : s_ctx.catalog.default_state;
}

bool behavior_state_is_busy(void) {
    bool busy = false;

    if (!s_ctx.initialized || !behavior_lock()) {
        return false;
    }

    busy = sfx_service_is_busy() ||
           (s_ctx.current_state_id[0] != '\0' && strcmp(s_ctx.current_state_id, s_ctx.catalog.default_state) != 0);
    behavior_unlock();
    return busy;
}
