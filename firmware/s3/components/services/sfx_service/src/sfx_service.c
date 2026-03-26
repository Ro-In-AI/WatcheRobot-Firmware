#include "sfx_service.h"

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal_audio.h"
#include "sensecap-watcher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "SFX_SERVICE"

#define SFX_MANIFEST_PATH "/spiffs/sfx/manifest.json"
#define SFX_DIR "/spiffs/sfx"
#define SFX_TASK_STACK 8192
#define SFX_TASK_PRIORITY 4
#define SFX_POLL_INTERVAL_MS 20
#define SFX_CHUNK_SIZE 2048
#define SFX_MAX_ID_LEN 32
#define SFX_MAX_PATH_LEN 128
#define SFX_MAX_MANIFEST_BYTES 8192

typedef struct {
    char id[SFX_MAX_ID_LEN];
    char path[SFX_MAX_PATH_LEN];
} sfx_manifest_entry_t;

typedef struct {
    sfx_manifest_entry_t *entries;
    int count;
} sfx_manifest_t;

typedef struct {
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    bool initialized;
    bool cloud_audio_busy;
    bool local_busy;
    bool stop_requested;
    uint32_t request_generation;
    char pending_sound_id[SFX_MAX_ID_LEN];
    sfx_manifest_t manifest;
} sfx_context_t;

static sfx_context_t s_ctx = {0};

static void sfx_copy_string(char *dst, size_t dst_size, const char *src) {
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

static void sfx_manifest_free(sfx_manifest_t *manifest) {
    if (manifest == NULL) {
        return;
    }

    free(manifest->entries);
    manifest->entries = NULL;
    manifest->count = 0;
}

static bool sfx_lock(void) {
    if (s_ctx.lock == NULL) {
        return false;
    }

    return xSemaphoreTake(s_ctx.lock, portMAX_DELAY) == pdTRUE;
}

static void sfx_unlock(void) {
    if (s_ctx.lock != NULL) {
        xSemaphoreGive(s_ctx.lock);
    }
}

static void sfx_normalize_path(const char *raw_path, char *out_path, size_t out_size) {
    if (out_path == NULL || out_size == 0) {
        return;
    }

    out_path[0] = '\0';
    if (raw_path == NULL || raw_path[0] == '\0') {
        return;
    }

    if (strncmp(raw_path, "/spiffs/", 8) == 0) {
        sfx_copy_string(out_path, out_size, raw_path);
        return;
    }

    if (strncmp(raw_path, "sfx/", 4) == 0) {
        snprintf(out_path, out_size, "/spiffs/%s", raw_path);
        return;
    }

    snprintf(out_path, out_size, "%s/%s", SFX_DIR, raw_path);
}

static char *sfx_read_text_file(const char *path, size_t max_bytes) {
    FILE *file = NULL;
    char *buffer = NULL;
    long file_size = 0;
    size_t read_size = 0;

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

static esp_err_t sfx_manifest_load_locked(void) {
    cJSON *root = NULL;
    cJSON *sounds = NULL;
    cJSON *item = NULL;
    sfx_manifest_t manifest = {0};
    char *json = sfx_read_text_file(SFX_MANIFEST_PATH, SFX_MAX_MANIFEST_BYTES);

    sfx_manifest_free(&s_ctx.manifest);
    if (json == NULL) {
        ESP_LOGI(TAG, "No sfx manifest found at %s, using direct file lookup", SFX_MANIFEST_PATH);
        return ESP_OK;
    }

    root = cJSON_Parse(json);
    free(json);
    if (root == NULL) {
        ESP_LOGW(TAG, "Failed to parse %s, using direct file lookup", SFX_MANIFEST_PATH);
        return ESP_FAIL;
    }

    sounds = cJSON_GetObjectItem(root, "sounds");
    if (sounds == NULL || !cJSON_IsObject(sounds)) {
        cJSON_Delete(root);
        return ESP_OK;
    }

    cJSON_ArrayForEach(item, sounds) {
        if (item->string != NULL) {
            manifest.count++;
        }
    }

    if (manifest.count > 0) {
        manifest.entries = (sfx_manifest_entry_t *)calloc((size_t)manifest.count, sizeof(sfx_manifest_entry_t));
        if (manifest.entries == NULL) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
    }

    manifest.count = 0;
    cJSON_ArrayForEach(item, sounds) {
        const char *path_value = NULL;

        if (item->string == NULL) {
            continue;
        }

        if (cJSON_IsString(item)) {
            path_value = item->valuestring;
        } else if (cJSON_IsObject(item)) {
            cJSON *path_item = cJSON_GetObjectItem(item, "path");
            if (path_item != NULL && cJSON_IsString(path_item)) {
                path_value = path_item->valuestring;
            }
        }

        if (path_value == NULL || path_value[0] == '\0') {
            continue;
        }

        sfx_copy_string(manifest.entries[manifest.count].id, sizeof(manifest.entries[manifest.count].id), item->string);
        sfx_normalize_path(path_value, manifest.entries[manifest.count].path, sizeof(manifest.entries[manifest.count].path));
        manifest.count++;
    }

    s_ctx.manifest = manifest;
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %d sfx manifest entries", s_ctx.manifest.count);
    return ESP_OK;
}

static void sfx_resolve_path_locked(const char *sound_id, char *out_path, size_t out_size) {
    int i;

    for (i = 0; i < s_ctx.manifest.count; ++i) {
        if (strcmp(s_ctx.manifest.entries[i].id, sound_id) == 0) {
            sfx_copy_string(out_path, out_size, s_ctx.manifest.entries[i].path);
            return;
        }
    }

    snprintf(out_path, out_size, "%s/%s.pcm", SFX_DIR, sound_id);
}

static bool sfx_sound_exists_locked(const char *sound_id, char *out_path, size_t out_size) {
    FILE *probe = NULL;
    char resolved_path[SFX_MAX_PATH_LEN];

    if (sound_id == NULL || sound_id[0] == '\0') {
        return false;
    }

    sfx_resolve_path_locked(sound_id, resolved_path, sizeof(resolved_path));
    probe = fopen(resolved_path, "rb");
    if (probe == NULL) {
        if (out_path != NULL && out_size > 0) {
            sfx_copy_string(out_path, out_size, resolved_path);
        }
        return false;
    }

    fclose(probe);
    if (out_path != NULL && out_size > 0) {
        sfx_copy_string(out_path, out_size, resolved_path);
    }
    return true;
}

static bool sfx_playback_should_abort(uint32_t expected_generation) {
    bool abort = true;

    if (!sfx_lock()) {
        return true;
    }

    abort = s_ctx.stop_requested || s_ctx.cloud_audio_busy || s_ctx.request_generation != expected_generation;
    sfx_unlock();
    return abort;
}

static void sfx_set_local_busy(bool busy) {
    if (!sfx_lock()) {
        return;
    }

    s_ctx.local_busy = busy;
    if (!busy) {
        s_ctx.stop_requested = false;
    }
    sfx_unlock();
}

static void sfx_playback_file(const char *sound_id, uint32_t generation) {
    FILE *file = NULL;
    uint8_t buffer[SFX_CHUNK_SIZE];
    char sound_path[SFX_MAX_PATH_LEN];
    bool audio_started = false;

    if (!sfx_lock()) {
        return;
    }
    sfx_resolve_path_locked(sound_id, sound_path, sizeof(sound_path));
    sfx_unlock();

    file = fopen(sound_path, "rb");
    if (file == NULL) {
        ESP_LOGI(TAG, "Skip local sfx '%s': file not found (%s)", sound_id, sound_path);
        return;
    }

    hal_audio_set_playback_mode(true);
    hal_audio_set_sample_rate(24000);
    if (hal_audio_start() != 0) {
        ESP_LOGW(TAG, "Failed to start audio for '%s'", sound_id);
        hal_audio_set_playback_mode(false);
        hal_audio_set_sample_rate(16000);
        fclose(file);
        return;
    }

    audio_started = true;
    sfx_set_local_busy(true);
    ESP_LOGI(TAG, "Playing local sfx '%s' from %s", sound_id, sound_path);

    while (!feof(file)) {
        size_t read_size;
        int written;

        if (sfx_playback_should_abort(generation)) {
            ESP_LOGI(TAG, "Stopping local sfx '%s' due to new audio request", sound_id);
            break;
        }

        read_size = fread(buffer, 1, sizeof(buffer), file);
        if (read_size == 0U) {
            break;
        }

        written = hal_audio_write(buffer, (int)read_size);
        if (written != (int)read_size) {
            ESP_LOGW(TAG, "Incomplete sfx playback '%s': %d/%u", sound_id, written, (unsigned int)read_size);
            break;
        }
    }

    fclose(file);
    if (audio_started) {
        hal_audio_stop();
        hal_audio_set_playback_mode(false);
        hal_audio_set_sample_rate(16000);
    }
    sfx_set_local_busy(false);
}

static bool sfx_take_pending_request(char *sound_id, size_t sound_id_size, uint32_t *generation) {
    bool has_request = false;

    if (!sfx_lock()) {
        return false;
    }

    if (!s_ctx.cloud_audio_busy && s_ctx.pending_sound_id[0] != '\0') {
        sfx_copy_string(sound_id, sound_id_size, s_ctx.pending_sound_id);
        s_ctx.pending_sound_id[0] = '\0';
        *generation = s_ctx.request_generation;
        has_request = true;
    }

    sfx_unlock();
    return has_request;
}

static void sfx_task(void *arg) {
    char sound_id[SFX_MAX_ID_LEN];
    uint32_t generation = 0;

    (void)arg;

    while (true) {
        if (sfx_take_pending_request(sound_id, sizeof(sound_id), &generation)) {
            sfx_playback_file(sound_id, generation);
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(SFX_POLL_INTERVAL_MS));
    }
}

esp_err_t sfx_service_init(void) {
    BaseType_t task_result;

    if (s_ctx.initialized) {
        return ESP_OK;
    }

    if (bsp_spiffs_init_default() != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed during sfx init");
    }

    s_ctx.lock = xSemaphoreCreateMutex();
    if (s_ctx.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (!sfx_lock()) {
        return ESP_FAIL;
    }
    sfx_manifest_load_locked();
    sfx_unlock();

    task_result = xTaskCreate(sfx_task, "sfx_task", SFX_TASK_STACK, NULL, SFX_TASK_PRIORITY, &s_ctx.task);
    if (task_result != pdPASS) {
        vSemaphoreDelete(s_ctx.lock);
        memset(&s_ctx, 0, sizeof(s_ctx));
        return ESP_ERR_NO_MEM;
    }

    s_ctx.initialized = true;
    return ESP_OK;
}

esp_err_t sfx_service_reload(void) {
    if (sfx_service_init() != ESP_OK) {
        return ESP_FAIL;
    }

    if (bsp_spiffs_init_default() != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed during sfx reload");
    }

    if (!sfx_lock()) {
        return ESP_FAIL;
    }

    sfx_manifest_load_locked();
    sfx_unlock();
    return ESP_OK;
}

esp_err_t sfx_service_play(const char *sound_id) {
    char sound_path[SFX_MAX_PATH_LEN];

    if (sound_id == NULL || sound_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (sfx_service_init() != ESP_OK) {
        return ESP_FAIL;
    }

    if (!sfx_lock()) {
        return ESP_FAIL;
    }

    if (s_ctx.cloud_audio_busy) {
        sfx_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    if (!sfx_sound_exists_locked(sound_id, sound_path, sizeof(sound_path))) {
        ESP_LOGI(TAG, "Skip local sfx '%s': no playable file at %s", sound_id, sound_path);
        sfx_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    s_ctx.request_generation++;
    if (s_ctx.request_generation == 0U) {
        s_ctx.request_generation = 1U;
    }
    s_ctx.stop_requested = false;
    sfx_copy_string(s_ctx.pending_sound_id, sizeof(s_ctx.pending_sound_id), sound_id);
    sfx_unlock();
    return ESP_OK;
}

void sfx_service_stop(void) {
    if (!s_ctx.initialized || !sfx_lock()) {
        return;
    }

    s_ctx.stop_requested = true;
    s_ctx.pending_sound_id[0] = '\0';
    s_ctx.request_generation++;
    if (s_ctx.request_generation == 0U) {
        s_ctx.request_generation = 1U;
    }
    sfx_unlock();
}

bool sfx_service_is_busy(void) {
    bool busy = false;

    if (!s_ctx.initialized || !sfx_lock()) {
        return false;
    }

    busy = s_ctx.local_busy || s_ctx.pending_sound_id[0] != '\0';
    sfx_unlock();
    return busy;
}

void sfx_service_set_cloud_audio_busy(bool busy) {
    if (sfx_service_init() != ESP_OK) {
        return;
    }

    if (!sfx_lock()) {
        return;
    }

    s_ctx.cloud_audio_busy = busy;
    if (busy) {
        s_ctx.stop_requested = true;
        s_ctx.pending_sound_id[0] = '\0';
        s_ctx.request_generation++;
        if (s_ctx.request_generation == 0U) {
            s_ctx.request_generation = 1U;
        }
    }
    sfx_unlock();
}

bool sfx_service_is_cloud_audio_busy(void) {
    bool busy = false;

    if (!s_ctx.initialized || !sfx_lock()) {
        return false;
    }

    busy = s_ctx.cloud_audio_busy;
    sfx_unlock();
    return busy;
}
