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

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TAG "BEHAVIOR_STATE"

#define BEHAVIOR_STATES_PATH "/spiffs/behavior/states.json"
#define BEHAVIOR_ACTIONS_DIR "/spiffs/actions"
#define BEHAVIOR_TASK_STACK 6144
#define BEHAVIOR_TASK_PRIORITY 5
#define BEHAVIOR_TICK_MS 10
#define BEHAVIOR_MAX_FILE_BYTES 16384
#define BEHAVIOR_ACTION_MAX_FILE_BYTES 32768
#define BEHAVIOR_VERSION_LEN 16
#define BEHAVIOR_STATE_ID_LEN 32
#define BEHAVIOR_TEXT_LEN 128
#define BEHAVIOR_SOUND_ID_LEN 32
#define BEHAVIOR_ACTION_PATH_LEN 160
#define BEHAVIOR_ACTION_DEFAULT_X_DEG 90
#define BEHAVIOR_ACTION_DEFAULT_Y_DEG 120
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
    int frame_number;
    int angle_deg;
} behavior_action_keyframe_t;

typedef struct {
    char id[BEHAVIOR_STATE_ID_LEN];
    uint32_t total_duration_ms;
    behavior_motion_event_t *motion;
    int motion_count;
} behavior_action_def_t;

typedef struct {
    behavior_action_def_t *actions;
    int action_count;
} behavior_action_catalog_t;

typedef struct {
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    bool initialized;
    behavior_catalog_t catalog;
    behavior_action_catalog_t action_catalog;
    behavior_state_def_t *current_state;
    behavior_action_def_t *current_action;
    char current_state_id[BEHAVIOR_STATE_ID_LEN];
    char current_action_id[BEHAVIOR_STATE_ID_LEN];
    uint32_t state_started_ms;
    int next_motion_index;
    int next_action_motion_index;
    int next_expression_index;
    int next_sound_index;
    char text_override[BEHAVIOR_TEXT_LEN];
    int text_override_font_size;
    bool text_override_valid;
    char anim_override[BEHAVIOR_STATE_ID_LEN];
    bool anim_override_valid;
    bool suppress_state_sound_events;
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

static int behavior_get_int(cJSON *obj, const char *key, int default_value) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item != NULL && cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return default_value;
}

static bool behavior_is_json_filename(const char *name) {
    size_t len;

    if (name == NULL) {
        return false;
    }

    len = strlen(name);
    return len > 5 && strcasecmp(&name[len - 5], ".json") == 0;
}

static void behavior_normalize_file_stem(const char *name, char *dst, size_t dst_size) {
    const char *base = name;
    const char *ext = NULL;
    size_t i;
    size_t len;

    if (dst == NULL || dst_size == 0) {
        return;
    }

    dst[0] = '\0';
    if (name == NULL || name[0] == '\0') {
        return;
    }

    for (i = 0; name[i] != '\0'; ++i) {
        if (name[i] == '/' || name[i] == '\\') {
            base = &name[i + 1];
        }
    }

    ext = strrchr(base, '.');
    len = (ext != NULL && ext > base) ? (size_t)(ext - base) : strlen(base);
    if (len >= dst_size) {
        len = dst_size - 1;
    }

    for (i = 0; i < len; ++i) {
        dst[i] = (char)tolower((unsigned char)base[i]);
    }
    dst[len] = '\0';
}

static int behavior_compare_keyframes(const void *lhs, const void *rhs) {
    const behavior_action_keyframe_t *left = (const behavior_action_keyframe_t *)lhs;
    const behavior_action_keyframe_t *right = (const behavior_action_keyframe_t *)rhs;

    if (left->frame_number < right->frame_number) {
        return -1;
    }
    if (left->frame_number > right->frame_number) {
        return 1;
    }
    return 0;
}

static int behavior_compare_ints(const void *lhs, const void *rhs) {
    const int left = *(const int *)lhs;
    const int right = *(const int *)rhs;

    if (left < right) {
        return -1;
    }
    if (left > right) {
        return 1;
    }
    return 0;
}

static int behavior_dedup_keyframes(behavior_action_keyframe_t *frames, int count) {
    int read_index;
    int write_index = 0;

    if (frames == NULL || count <= 0) {
        return 0;
    }

    qsort(frames, (size_t)count, sizeof(behavior_action_keyframe_t), behavior_compare_keyframes);
    for (read_index = 0; read_index < count; ++read_index) {
        if (write_index > 0 && frames[write_index - 1].frame_number == frames[read_index].frame_number) {
            frames[write_index - 1] = frames[read_index];
        } else {
            frames[write_index++] = frames[read_index];
        }
    }

    return write_index;
}

static bool behavior_append_keyframe(behavior_action_keyframe_t **frames,
                                     int *count,
                                     int *capacity,
                                     int frame_number,
                                     int angle_deg) {
    behavior_action_keyframe_t *new_frames = NULL;

    if (frames == NULL || count == NULL || capacity == NULL) {
        return false;
    }
    if (angle_deg < 0 || angle_deg > 180) {
        return false;
    }

    if (*count >= *capacity) {
        int new_capacity = (*capacity == 0) ? 8 : (*capacity * 2);
        new_frames =
            (behavior_action_keyframe_t *)realloc(*frames, (size_t)new_capacity * sizeof(behavior_action_keyframe_t));
        if (new_frames == NULL) {
            return false;
        }

        *frames = new_frames;
        *capacity = new_capacity;
    }

    (*frames)[*count].frame_number = frame_number;
    (*frames)[*count].angle_deg = angle_deg;
    (*count)++;
    return true;
}

static int behavior_find_angle_for_frame(const behavior_action_keyframe_t *frames, int count, int frame_number, int fallback) {
    int index;
    int angle = fallback;

    if (frames == NULL || count <= 0) {
        return fallback;
    }

    angle = frames[0].angle_deg;
    for (index = 0; index < count; ++index) {
        if (frames[index].frame_number > frame_number) {
            break;
        }
        angle = frames[index].angle_deg;
    }

    return angle;
}

static uint32_t behavior_action_frame_to_ms(int frame_number, int start_frame, int fps) {
    int relative_frame = frame_number - start_frame;

    if (fps <= 0) {
        return 0;
    }
    if (relative_frame <= 0) {
        return 0;
    }

    return (uint32_t)(((int64_t)relative_frame * 1000 + (fps / 2)) / fps);
}

static void behavior_free_action_catalog(behavior_action_catalog_t *catalog) {
    int i;

    if (catalog == NULL) {
        return;
    }

    if (catalog->actions != NULL) {
        for (i = 0; i < catalog->action_count; ++i) {
            free(catalog->actions[i].motion);
        }
    }

    free(catalog->actions);
    memset(catalog, 0, sizeof(*catalog));
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

static behavior_action_def_t *behavior_find_action_locked(const char *action_id) {
    int i;

    if (action_id == NULL || action_id[0] == '\0') {
        return NULL;
    }

    for (i = 0; i < s_ctx.action_catalog.action_count; ++i) {
        if (strcmp(s_ctx.action_catalog.actions[i].id, action_id) == 0) {
            return &s_ctx.action_catalog.actions[i];
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

static bool behavior_append_frame_number(int **frames, int *count, int *capacity, int frame_number) {
    int *new_frames = NULL;

    if (frames == NULL || count == NULL || capacity == NULL) {
        return false;
    }

    if (*count >= *capacity) {
        int new_capacity = (*capacity == 0) ? 8 : (*capacity * 2);
        new_frames = (int *)realloc(*frames, (size_t)new_capacity * sizeof(int));
        if (new_frames == NULL) {
            return false;
        }

        *frames = new_frames;
        *capacity = new_capacity;
    }

    (*frames)[*count] = frame_number;
    (*count)++;
    return true;
}

static int behavior_dedup_frame_numbers(int *frames, int count) {
    int read_index;
    int write_index = 0;

    if (frames == NULL || count <= 0) {
        return 0;
    }

    qsort(frames, (size_t)count, sizeof(int), behavior_compare_ints);
    for (read_index = 0; read_index < count; ++read_index) {
        if (write_index == 0 || frames[write_index - 1] != frames[read_index]) {
            frames[write_index++] = frames[read_index];
        }
    }

    return write_index;
}

static esp_err_t behavior_parse_action_file(const char *action_id,
                                            const char *path,
                                            behavior_action_def_t *out_action) {
    char *json = NULL;
    cJSON *root = NULL;
    cJSON *animated_objects = NULL;
    cJSON *object = NULL;
    behavior_action_keyframe_t *x_frames = NULL;
    behavior_action_keyframe_t *y_frames = NULL;
    behavior_motion_event_t *events = NULL;
    int *frame_numbers = NULL;
    int x_count = 0;
    int x_capacity = 0;
    int y_count = 0;
    int y_capacity = 0;
    int frame_count = 0;
    int frame_capacity = 0;
    int fps;
    int frame_start;
    int frame_end;
    int effective_start;
    int effective_end;
    int max_keyframe = 0;
    int event_count = 0;
    int index;
    int last_x = BEHAVIOR_ACTION_DEFAULT_X_DEG;
    int last_y = BEHAVIOR_ACTION_DEFAULT_Y_DEG;

    if (action_id == NULL || action_id[0] == '\0' || path == NULL || out_action == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_action, 0, sizeof(*out_action));
    behavior_copy_string(out_action->id, sizeof(out_action->id), action_id);

    json = behavior_read_text_file(path, BEHAVIOR_ACTION_MAX_FILE_BYTES);
    if (json == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    root = cJSON_Parse(json);
    free(json);
    if (root == NULL) {
        return ESP_FAIL;
    }

    fps = behavior_get_int(root, "fps", 0);
    frame_start = behavior_get_int(root, "frame_start", 0);
    frame_end = behavior_get_int(root, "frame_end", frame_start);
    animated_objects = cJSON_GetObjectItem(root, "animated_objects");
    if (fps <= 0 || animated_objects == NULL || !cJSON_IsArray(animated_objects)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON_ArrayForEach(object, animated_objects) {
        cJSON *keyframe_data = NULL;
        cJSON *keyframe = NULL;

        if (!cJSON_IsObject(object)) {
            continue;
        }

        keyframe_data = cJSON_GetObjectItem(object, "keyframe_data");
        if (keyframe_data == NULL || !cJSON_IsArray(keyframe_data)) {
            continue;
        }

        cJSON_ArrayForEach(keyframe, keyframe_data) {
            const char *axis_name;
            cJSON *rotation_item = NULL;
            int frame_number;
            int angle_deg;

            if (!cJSON_IsObject(keyframe)) {
                continue;
            }

            axis_name = behavior_get_string(keyframe, "active_axis");
            rotation_item = cJSON_GetObjectItem(keyframe, "rotation_angle");
            frame_number = behavior_get_int(keyframe, "frame_number", frame_start);
            if (axis_name == NULL || rotation_item == NULL || !cJSON_IsNumber(rotation_item)) {
                continue;
            }

            angle_deg = (int)(rotation_item->valuedouble >= 0.0 ? (rotation_item->valuedouble + 0.5)
                                                                : (rotation_item->valuedouble - 0.5));
            if (frame_number > max_keyframe) {
                max_keyframe = frame_number;
            }

            if (strcasecmp(axis_name, "z") == 0) {
                if (!behavior_append_keyframe(&x_frames, &x_count, &x_capacity, frame_number, angle_deg)) {
                    cJSON_Delete(root);
                    free(x_frames);
                    free(y_frames);
                    free(frame_numbers);
                    return ESP_ERR_NO_MEM;
                }
            } else if (strcasecmp(axis_name, "x") == 0) {
                if (!behavior_append_keyframe(&y_frames, &y_count, &y_capacity, frame_number, angle_deg)) {
                    cJSON_Delete(root);
                    free(x_frames);
                    free(y_frames);
                    free(frame_numbers);
                    return ESP_ERR_NO_MEM;
                }
            }
        }
    }

    x_count = behavior_dedup_keyframes(x_frames, x_count);
    y_count = behavior_dedup_keyframes(y_frames, y_count);

    if (x_count > 0) {
        last_x = x_frames[0].angle_deg;
        effective_start = x_frames[0].frame_number;
    } else if (y_count > 0) {
        effective_start = y_frames[0].frame_number;
    } else {
        effective_start = frame_start;
    }

    if (y_count > 0) {
        last_y = y_frames[0].angle_deg;
        if (y_frames[0].frame_number < effective_start) {
            effective_start = y_frames[0].frame_number;
        }
    }
    if (frame_start < effective_start) {
        effective_start = frame_start;
    }

    effective_end = frame_end;
    if (max_keyframe > effective_end) {
        effective_end = max_keyframe;
    }
    if (effective_end < effective_start) {
        effective_end = effective_start;
    }

    for (index = 0; index < x_count; ++index) {
        if (!behavior_append_frame_number(&frame_numbers, &frame_count, &frame_capacity, x_frames[index].frame_number)) {
            cJSON_Delete(root);
            free(x_frames);
            free(y_frames);
            free(frame_numbers);
            return ESP_ERR_NO_MEM;
        }
    }
    for (index = 0; index < y_count; ++index) {
        if (!behavior_append_frame_number(&frame_numbers, &frame_count, &frame_capacity, y_frames[index].frame_number)) {
            cJSON_Delete(root);
            free(x_frames);
            free(y_frames);
            free(frame_numbers);
            return ESP_ERR_NO_MEM;
        }
    }

    frame_count = behavior_dedup_frame_numbers(frame_numbers, frame_count);
    if (frame_count > 0) {
        events = (behavior_motion_event_t *)calloc((size_t)frame_count, sizeof(behavior_motion_event_t));
        if (events == NULL) {
            cJSON_Delete(root);
            free(x_frames);
            free(y_frames);
            free(frame_numbers);
            return ESP_ERR_NO_MEM;
        }
    }

    for (index = 0; index < frame_count; ++index) {
        int current_frame = frame_numbers[index];
        int next_frame = (index + 1 < frame_count) ? frame_numbers[index + 1] : effective_end;
        int x_deg = behavior_find_angle_for_frame(x_frames, x_count, current_frame, last_x);
        int y_deg = behavior_find_angle_for_frame(y_frames, y_count, current_frame, last_y);
        uint32_t at_ms = behavior_action_frame_to_ms(current_frame, effective_start, fps);
        uint32_t next_ms = behavior_action_frame_to_ms(next_frame, effective_start, fps);
        int duration_ms;

        last_x = x_deg;
        last_y = y_deg;
        duration_ms = (next_ms > at_ms) ? (int)(next_ms - at_ms) : 0;
        if (event_count > 0 && events[event_count - 1].x_deg == x_deg && events[event_count - 1].y_deg == y_deg) {
            continue;
        }

        events[event_count].at_ms = at_ms;
        events[event_count].x_deg = x_deg;
        events[event_count].y_deg = y_deg;
        events[event_count].duration_ms = duration_ms;
        event_count++;
    }

    out_action->motion = events;
    out_action->motion_count = event_count;
    out_action->total_duration_ms = behavior_action_frame_to_ms(effective_end, effective_start, fps);
    if (out_action->total_duration_ms == 0 && event_count > 0) {
        out_action->total_duration_ms =
            events[event_count - 1].at_ms + (uint32_t)events[event_count - 1].duration_ms;
    }

    cJSON_Delete(root);
    free(x_frames);
    free(y_frames);
    free(frame_numbers);
    return ESP_OK;
}

static esp_err_t behavior_load_actions_from_dir(behavior_action_catalog_t *out_catalog) {
    behavior_action_catalog_t catalog = {0};
    DIR *dir = NULL;
    struct dirent *entry = NULL;
    int action_count = 0;
    int index = 0;

    if (out_catalog == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    dir = opendir(BEHAVIOR_ACTIONS_DIR);
    if (dir == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (behavior_is_json_filename(entry->d_name)) {
            action_count++;
        }
    }
    closedir(dir);

    if (action_count <= 0) {
        *out_catalog = catalog;
        return ESP_OK;
    }

    catalog.actions = (behavior_action_def_t *)calloc((size_t)action_count, sizeof(behavior_action_def_t));
    if (catalog.actions == NULL) {
        return ESP_ERR_NO_MEM;
    }

    dir = opendir(BEHAVIOR_ACTIONS_DIR);
    if (dir == NULL) {
        behavior_free_action_catalog(&catalog);
        return ESP_ERR_NOT_FOUND;
    }

    while ((entry = readdir(dir)) != NULL) {
        char action_id[BEHAVIOR_STATE_ID_LEN];
        char path[BEHAVIOR_ACTION_PATH_LEN];
        size_t dir_len;
        size_t name_len;
        esp_err_t ret;

        if (!behavior_is_json_filename(entry->d_name)) {
            continue;
        }

        behavior_normalize_file_stem(entry->d_name, action_id, sizeof(action_id));
        dir_len = strlen(BEHAVIOR_ACTIONS_DIR);
        name_len = strlen(entry->d_name);
        if (dir_len + 1 + name_len >= sizeof(path)) {
            ESP_LOGE(TAG, "Action path too long: %s/%s", BEHAVIOR_ACTIONS_DIR, entry->d_name);
            closedir(dir);
            behavior_free_action_catalog(&catalog);
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(path, BEHAVIOR_ACTIONS_DIR, dir_len);
        path[dir_len] = '/';
        memcpy(path + dir_len + 1, entry->d_name, name_len);
        path[dir_len + 1 + name_len] = '\0';
        ret = behavior_parse_action_file(action_id, path, &catalog.actions[index]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to parse action '%s': %s", path, esp_err_to_name(ret));
            closedir(dir);
            behavior_free_action_catalog(&catalog);
            return ret;
        }

        ESP_LOGI(TAG,
                 "Loaded action '%s': motion_count=%d duration_ms=%lu",
                 catalog.actions[index].id,
                 catalog.actions[index].motion_count,
                 (unsigned long)catalog.actions[index].total_duration_ms);
        index++;
    }
    closedir(dir);

    catalog.action_count = index;
    *out_catalog = catalog;
    return ESP_OK;
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
    s_ctx.current_action = NULL;
    behavior_copy_string(s_ctx.current_state_id, sizeof(s_ctx.current_state_id), s_ctx.catalog.default_state);
    s_ctx.current_action_id[0] = '\0';
    s_ctx.state_started_ms = behavior_now_ms();
    s_ctx.next_motion_index = 0;
    s_ctx.next_action_motion_index = 0;
    s_ctx.next_expression_index = 0;
    s_ctx.next_sound_index = 0;
    s_ctx.text_override[0] = '\0';
    s_ctx.text_override_font_size = 0;
    s_ctx.text_override_valid = false;
    s_ctx.anim_override[0] = '\0';
    s_ctx.anim_override_valid = false;
    s_ctx.suppress_state_sound_events = false;
}

static bool behavior_is_valid_anim_id(const char *anim_id) {
    return anim_id != NULL && anim_id[0] != '\0' && display_emoji_from_string(anim_id) != EMOJI_UNKNOWN;
}

static bool behavior_is_same_state_action_request_locked(const char *state_id, const char *action_id) {
    if (state_id == NULL || state_id[0] == '\0' || strcmp(state_id, s_ctx.current_state_id) != 0) {
        return false;
    }

    if (action_id == NULL || action_id[0] == '\0') {
        return s_ctx.current_action_id[0] == '\0';
    }

    return strcmp(action_id, s_ctx.current_action_id) == 0;
}

static void behavior_set_anim_override_locked(const char *anim_id) {
    if (behavior_is_valid_anim_id(anim_id)) {
        behavior_copy_string(s_ctx.anim_override, sizeof(s_ctx.anim_override), anim_id);
        s_ctx.anim_override_valid = true;
    } else {
        s_ctx.anim_override[0] = '\0';
        s_ctx.anim_override_valid = false;
    }
}

static void behavior_get_display_defaults_locked(const char **text, const char **anim, int *font_size) {
    if (text != NULL) {
        *text = NULL;
    }
    if (anim != NULL) {
        *anim = NULL;
    }
    if (font_size != NULL) {
        *font_size = 0;
    }

    if (s_ctx.current_state != NULL && s_ctx.current_state->expression_count > 0) {
        if (text != NULL && s_ctx.current_state->expression[0].text[0] != '\0') {
            *text = s_ctx.current_state->expression[0].text;
        }
        if (anim != NULL && s_ctx.current_state->expression[0].anim[0] != '\0') {
            *anim = s_ctx.current_state->expression[0].anim;
        }
        if (font_size != NULL) {
            *font_size = s_ctx.current_state->expression[0].font_size;
        }
    } else if (anim != NULL) {
        *anim = s_ctx.current_state_id[0] != '\0' ? s_ctx.current_state_id : s_ctx.catalog.default_state;
    }
}

static void behavior_refresh_display_locked(void) {
    const char *text = NULL;
    const char *anim = NULL;
    int font_size = 0;

    behavior_get_display_defaults_locked(&text, &anim, &font_size);
    if (s_ctx.text_override_valid) {
        text = s_ctx.text_override;
        font_size = s_ctx.text_override_font_size;
    }
    if (s_ctx.anim_override_valid) {
        anim = s_ctx.anim_override;
    }

    if (display_update(text, anim, font_size, NULL) != 0) {
        ESP_LOGW(TAG, "Display refresh failed for state '%s'", s_ctx.current_state_id);
    }
}

static bool behavior_should_override_state_motion_locked(void) {
    return s_ctx.current_action != NULL;
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
    const char *anim = NULL;
    const char *text = NULL;
    int font_size = 0;

    if (event == NULL) {
        return;
    }

    if (s_ctx.anim_override_valid) {
        anim = s_ctx.anim_override;
    } else if (event->anim[0] != '\0') {
        anim = event->anim;
    }

    if (s_ctx.text_override_valid) {
        text = s_ctx.text_override;
        font_size = s_ctx.text_override_font_size;
    } else if (event->text[0] != '\0') {
        text = event->text;
        font_size = event->font_size;
    }

    if (display_update(text, anim, font_size, NULL) != 0) {
        ESP_LOGW(TAG, "Display update failed for state '%s'", s_ctx.current_state_id);
    }
}

static esp_err_t behavior_dispatch_sound_id_locked(const char *sound_id) {
    esp_err_t ret;

    if (sound_id == NULL || sound_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ret = sfx_service_play(sound_id);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Skip sound '%s': audio_busy_tts", sound_id);
    } else if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to play sound '%s': %s", sound_id, esp_err_to_name(ret));
    }

    return ret;
}

static void behavior_dispatch_sound_locked(const behavior_sound_event_t *event) {
    if (event == NULL) {
        return;
    }

    (void)behavior_dispatch_sound_id_locked(event->sound_id);
}

static bool behavior_apply_sound_override_locked(const char *sound_id) {
    esp_err_t ret;

    if (sound_id == NULL || sound_id[0] == '\0') {
        return false;
    }

    ret = behavior_dispatch_sound_id_locked(sound_id);
    return ret == ESP_OK || ret == ESP_ERR_INVALID_STATE;
}

static bool behavior_all_state_events_dispatched_locked(void) {
    int motion_count = 0;

    if (s_ctx.current_state == NULL) {
        return false;
    }

    motion_count = behavior_should_override_state_motion_locked() ? 0 : s_ctx.current_state->motion_count;
    return s_ctx.next_motion_index >= motion_count && s_ctx.next_expression_index >= s_ctx.current_state->expression_count &&
           s_ctx.next_sound_index >= s_ctx.current_state->sound_count;
}

static bool behavior_all_action_events_dispatched_locked(void) {
    return s_ctx.current_action == NULL || s_ctx.next_action_motion_index >= s_ctx.current_action->motion_count;
}

static void behavior_dispatch_due_events_locked(uint32_t now_ms) {
    uint32_t elapsed_ms;

    if (s_ctx.current_state == NULL) {
        return;
    }

    elapsed_ms = now_ms - s_ctx.state_started_ms;

    if (!behavior_should_override_state_motion_locked()) {
        while (s_ctx.next_motion_index < s_ctx.current_state->motion_count &&
               s_ctx.current_state->motion[s_ctx.next_motion_index].at_ms <= elapsed_ms) {
            behavior_dispatch_motion_locked(&s_ctx.current_state->motion[s_ctx.next_motion_index]);
            s_ctx.next_motion_index++;
        }
    }

    while (s_ctx.current_action != NULL && s_ctx.next_action_motion_index < s_ctx.current_action->motion_count &&
           s_ctx.current_action->motion[s_ctx.next_action_motion_index].at_ms <= elapsed_ms) {
        behavior_dispatch_motion_locked(&s_ctx.current_action->motion[s_ctx.next_action_motion_index]);
        s_ctx.next_action_motion_index++;
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

    if (s_ctx.current_state->expression_count == 0 && (s_ctx.text_override_valid || s_ctx.anim_override_valid)) {
        behavior_refresh_display_locked();
    }
}

static uint32_t behavior_non_loop_done_at_ms_locked(void) {
    uint32_t done_at_ms = 0;

    if (s_ctx.current_state == NULL) {
        return 0;
    }

    done_at_ms = s_ctx.current_state->timeline_end_ms > 0 ? s_ctx.current_state->timeline_end_ms
                                                          : BEHAVIOR_DEFAULT_ONESHOT_HOLD_MS;
    if (s_ctx.current_action != NULL && s_ctx.current_action->total_duration_ms > done_at_ms) {
        done_at_ms = s_ctx.current_action->total_duration_ms;
    }

    return done_at_ms;
}

static esp_err_t behavior_schedule_state_locked(const char *state_id,
                                                const char *text,
                                                int font_size,
                                                const char *anim_id,
                                                const char *sound_id,
                                                const char *action_id) {
    behavior_state_def_t *state_def = behavior_find_state_locked(state_id);
    behavior_action_def_t *action_def = behavior_find_action_locked(action_id);
    const char *effective_state_id = NULL;
    uint32_t now_ms = behavior_now_ms();

    if (state_def == NULL) {
        if (strcmp(state_id, s_ctx.catalog.default_state) != 0 && strcmp(state_id, "standby") != 0) {
            return ESP_ERR_NOT_FOUND;
        }

        effective_state_id = s_ctx.catalog.default_state;
        if (behavior_is_same_state_action_request_locked(effective_state_id, action_def != NULL ? action_def->id : NULL)) {
            s_ctx.text_override_valid = (text != NULL);
            behavior_copy_string(s_ctx.text_override, sizeof(s_ctx.text_override), text);
            s_ctx.text_override_font_size = font_size;
            behavior_set_anim_override_locked(anim_id);
            (void)behavior_apply_sound_override_locked(sound_id);
            if (display_update(s_ctx.text_override_valid ? s_ctx.text_override : NULL,
                               s_ctx.anim_override_valid ? s_ctx.anim_override : s_ctx.catalog.default_state,
                               s_ctx.text_override_font_size,
                               NULL) != 0) {
                ESP_LOGW(TAG, "Fallback standby display update failed");
            }
            return ESP_OK;
        }

        s_ctx.current_state = NULL;
        s_ctx.current_action = action_def;
        behavior_copy_string(s_ctx.current_state_id, sizeof(s_ctx.current_state_id), s_ctx.catalog.default_state);
        behavior_copy_string(s_ctx.current_action_id,
                             sizeof(s_ctx.current_action_id),
                             action_def != NULL ? action_def->id : NULL);
        s_ctx.state_started_ms = now_ms;
        s_ctx.next_motion_index = 0;
        s_ctx.next_action_motion_index = 0;
        s_ctx.next_expression_index = 0;
        s_ctx.next_sound_index = 0;
        s_ctx.text_override_valid = (text != NULL);
        behavior_copy_string(s_ctx.text_override, sizeof(s_ctx.text_override), text);
        s_ctx.text_override_font_size = font_size;
        behavior_set_anim_override_locked(anim_id);
        s_ctx.suppress_state_sound_events = false;
        (void)behavior_apply_sound_override_locked(sound_id);
        if (display_update(text,
                           s_ctx.anim_override_valid ? s_ctx.anim_override : s_ctx.catalog.default_state,
                           font_size,
                           NULL) != 0) {
            ESP_LOGW(TAG, "Fallback standby display update failed");
        }
        return ESP_OK;
    }

    effective_state_id = state_def->id;
    if (behavior_is_same_state_action_request_locked(effective_state_id, action_def != NULL ? action_def->id : NULL)) {
        s_ctx.text_override_valid = (text != NULL);
        behavior_copy_string(s_ctx.text_override, sizeof(s_ctx.text_override), text);
        s_ctx.text_override_font_size = font_size;
        behavior_set_anim_override_locked(anim_id);
        if (behavior_apply_sound_override_locked(sound_id)) {
            s_ctx.suppress_state_sound_events = true;
            s_ctx.next_sound_index = s_ctx.current_state != NULL ? s_ctx.current_state->sound_count : 0;
        }
        behavior_refresh_display_locked();
        return ESP_OK;
    }

    s_ctx.current_state = state_def;
    s_ctx.current_action = action_def;
    behavior_copy_string(s_ctx.current_state_id, sizeof(s_ctx.current_state_id), state_def->id);
    behavior_copy_string(s_ctx.current_action_id,
                         sizeof(s_ctx.current_action_id),
                         action_def != NULL ? action_def->id : NULL);
    s_ctx.state_started_ms = now_ms;
    s_ctx.next_motion_index = 0;
    s_ctx.next_action_motion_index = 0;
    s_ctx.next_expression_index = 0;
    s_ctx.next_sound_index = 0;
    s_ctx.text_override_valid = (text != NULL);
    behavior_copy_string(s_ctx.text_override, sizeof(s_ctx.text_override), text);
    s_ctx.text_override_font_size = font_size;
    behavior_set_anim_override_locked(anim_id);
    s_ctx.suppress_state_sound_events = behavior_apply_sound_override_locked(sound_id);
    if (s_ctx.suppress_state_sound_events) {
        s_ctx.next_sound_index = s_ctx.current_state->sound_count;
    }
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
                    if (s_ctx.current_state->timeline_end_ms > 0 && behavior_all_state_events_dispatched_locked() &&
                        behavior_all_action_events_dispatched_locked() && elapsed_ms >= s_ctx.current_state->timeline_end_ms &&
                        (s_ctx.current_action == NULL || elapsed_ms >= s_ctx.current_action->total_duration_ms)) {
                        s_ctx.state_started_ms = now_ms;
                        s_ctx.next_motion_index = 0;
                        s_ctx.next_expression_index = 0;
                        s_ctx.next_sound_index =
                            s_ctx.suppress_state_sound_events ? s_ctx.current_state->sound_count : 0;
                        behavior_dispatch_due_events_locked(now_ms);
                    }
                } else {
                    done_at_ms = behavior_non_loop_done_at_ms_locked();
                    if (behavior_all_state_events_dispatched_locked() && behavior_all_action_events_dispatched_locked() &&
                        elapsed_ms >= done_at_ms) {
                        behavior_copy_string(fallback_state, sizeof(fallback_state), s_ctx.catalog.default_state);
                        s_ctx.current_state = NULL;
                        s_ctx.current_action = NULL;
                        s_ctx.next_motion_index = 0;
                        s_ctx.next_action_motion_index = 0;
                        s_ctx.next_expression_index = 0;
                        s_ctx.next_sound_index = 0;
                        s_ctx.current_action_id[0] = '\0';
                        s_ctx.text_override[0] = '\0';
                        s_ctx.text_override_font_size = 0;
                        s_ctx.text_override_valid = false;
                        s_ctx.anim_override[0] = '\0';
                        s_ctx.anim_override_valid = false;
                        s_ctx.suppress_state_sound_events = false;
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
    behavior_action_catalog_t action_catalog = {0};
    esp_err_t ret;
    esp_err_t action_ret;

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
            behavior_free_action_catalog(&s_ctx.action_catalog);
            behavior_copy_string(s_ctx.catalog.version, sizeof(s_ctx.catalog.version), "1.0");
            behavior_copy_string(s_ctx.catalog.default_state, sizeof(s_ctx.catalog.default_state), "standby");
            behavior_reset_runtime_locked();
            behavior_unlock();
        }
        return ret;
    }

    action_ret = behavior_load_actions_from_dir(&action_catalog);
    if (action_ret != ESP_OK && action_ret != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "Behavior action load failed: %s", esp_err_to_name(action_ret));
        behavior_free_action_catalog(&action_catalog);
        memset(&action_catalog, 0, sizeof(action_catalog));
    }

    if (behavior_lock()) {
        behavior_free_catalog(&s_ctx.catalog);
        behavior_free_action_catalog(&s_ctx.action_catalog);
        s_ctx.catalog = catalog;
        s_ctx.action_catalog = action_catalog;
        behavior_reset_runtime_locked();
        behavior_unlock();
    } else {
        behavior_free_catalog(&catalog);
        behavior_free_action_catalog(&action_catalog);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "Loaded behavior states v%s: count=%d default=%s actions=%d",
             s_ctx.catalog.version,
             s_ctx.catalog.state_count,
             s_ctx.catalog.default_state,
             s_ctx.action_catalog.action_count);
    return ESP_OK;
}

esp_err_t behavior_state_set(const char *state_id) {
    return behavior_state_set_with_resources(state_id, NULL, 0, NULL, NULL);
}

esp_err_t behavior_state_set_with_text(const char *state_id, const char *text, int font_size) {
    return behavior_state_set_with_resources(state_id, text, font_size, NULL, NULL);
}

esp_err_t behavior_state_set_with_resources(const char *state_id,
                                            const char *text,
                                            int font_size,
                                            const char *anim_id,
                                            const char *sound_id) {
    return behavior_state_set_with_resources_and_action(state_id, text, font_size, anim_id, sound_id, NULL);
}

esp_err_t behavior_state_set_with_resources_and_action(const char *state_id,
                                                       const char *text,
                                                       int font_size,
                                                       const char *anim_id,
                                                       const char *sound_id,
                                                       const char *action_id) {
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

    ret = behavior_schedule_state_locked(state_id, text, font_size, anim_id, sound_id, action_id);
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

bool behavior_state_has_action(const char *action_id) {
    bool has_action = false;

    if (action_id == NULL || action_id[0] == '\0') {
        return false;
    }
    if (behavior_state_init() != ESP_OK) {
        return false;
    }
    if (!behavior_lock()) {
        return false;
    }

    has_action = behavior_find_action_locked(action_id) != NULL;
    behavior_unlock();
    return has_action;
}

bool behavior_state_is_action_active(void) {
    bool active = false;
    uint32_t elapsed_ms = 0;

    if (behavior_state_init() != ESP_OK) {
        return false;
    }
    if (!behavior_lock()) {
        return false;
    }

    if (s_ctx.current_action != NULL && s_ctx.current_action->total_duration_ms > 0) {
        elapsed_ms = behavior_now_ms() - s_ctx.state_started_ms;
        active = elapsed_ms < s_ctx.current_action->total_duration_ms;
    }

    behavior_unlock();
    return active;
}
