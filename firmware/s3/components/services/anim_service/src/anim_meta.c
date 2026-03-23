/**
 * @file anim_meta.c
 * @brief Animation metadata configuration implementation
 */

#include "anim_meta.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

#define TAG "ANIM_META"

/* Metadata file path */
#define ANIM_META_PATH "/spiffs/anim/anim_meta.json"

/* Default values */
#define DEFAULT_FPS 30
#define DEFAULT_LOOP true

/* Metadata state */
static anim_meta_t g_meta;
static bool g_meta_loaded = false;

/* Animation type names for JSON parsing */
static const char *anim_type_names[EMOJI_ANIM_COUNT] = {
    "boot",
    "happy",
    "error",
    "speaking",
    "listening",
    "processing",
    "standby",
    "thinking",
    "custom1",
    "custom2",
    "custom3",
};

static void set_defaults(void) {
    memset(&g_meta, 0, sizeof(g_meta));
    strncpy(g_meta.version, "1.0", sizeof(g_meta.version) - 1);
    g_meta.default_fps = DEFAULT_FPS;

    for (int i = 0; i < EMOJI_ANIM_COUNT; i++) {
        g_meta.animations[i].frame_count = 0; /* 0 = auto-detect */
        g_meta.animations[i].loop = DEFAULT_LOOP;
        g_meta.animations[i].fps = 0; /* 0 = use default */
    }
}

static int parse_anim_config(cJSON *obj, anim_config_t *config) {
    if (obj == NULL || config == NULL) {
        return -1;
    }

    cJSON *item;

    item = cJSON_GetObjectItem(obj, "frame_count");
    if (item != NULL && cJSON_IsNumber(item)) {
        config->frame_count = item->valueint;
    }

    item = cJSON_GetObjectItem(obj, "loop");
    if (item != NULL && cJSON_IsBool(item)) {
        config->loop = cJSON_IsTrue(item);
    }

    item = cJSON_GetObjectItem(obj, "fps");
    if (item != NULL && cJSON_IsNumber(item)) {
        config->fps = item->valueint;
    }

    return 0;
}

static int load_meta_from_file(void) {
    FILE *f = fopen(ANIM_META_PATH, "r");
    if (f == NULL) {
        ESP_LOGI(TAG, "No metadata file found at %s, using defaults", ANIM_META_PATH);
        return -1;
    }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0 || file_size > 8192) {
        ESP_LOGW(TAG, "Invalid metadata file size: %ld", file_size);
        fclose(f);
        return -1;
    }

    /* Allocate buffer in internal RAM (small file) */
    char *json_str = (char *)malloc(file_size + 1);
    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to allocate buffer for metadata");
        fclose(f);
        return -1;
    }

    /* Read file */
    size_t read_size = fread(json_str, 1, file_size, f);
    fclose(f);
    json_str[read_size] = '\0';

    /* Parse JSON */
    cJSON *root = cJSON_Parse(json_str);
    free(json_str);

    if (root == NULL) {
        ESP_LOGW(TAG, "Failed to parse metadata JSON");
        return -1;
    }

    /* Parse version */
    cJSON *item = cJSON_GetObjectItem(root, "version");
    if (item != NULL && cJSON_IsString(item)) {
        strncpy(g_meta.version, item->valuestring, sizeof(g_meta.version) - 1);
    }

    /* Parse default FPS */
    item = cJSON_GetObjectItem(root, "default_fps");
    if (item != NULL && cJSON_IsNumber(item)) {
        g_meta.default_fps = item->valueint;
    }

    /* Parse per-animation configs */
    cJSON *animations = cJSON_GetObjectItem(root, "animations");
    if (animations != NULL && cJSON_IsObject(animations)) {
        for (int i = 0; i < EMOJI_ANIM_COUNT; i++) {
            cJSON *anim_obj = cJSON_GetObjectItem(animations, anim_type_names[i]);
            if (anim_obj != NULL) {
                parse_anim_config(anim_obj, &g_meta.animations[i]);
            }
        }
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded metadata v%s, default_fps=%d", g_meta.version, g_meta.default_fps);

    return 0;
}

int anim_meta_init(void) {
    /* Don't re-initialize if already loaded */
    if (g_meta_loaded) {
        ESP_LOGD(TAG, "Metadata already loaded, skipping init");
        return 0;
    }

    /* Start with defaults */
    set_defaults();

    /* Try to load from file */
    load_meta_from_file();

    g_meta_loaded = true;
    return 0;
}

anim_config_t *anim_meta_get_config(emoji_anim_type_t type) {
    if (type < 0 || type >= EMOJI_ANIM_COUNT) {
        static anim_config_t fallback = {0, true, 0};
        return &fallback;
    }
    return &g_meta.animations[type];
}

int anim_meta_get_fps(emoji_anim_type_t type) {
    if (type < 0 || type >= EMOJI_ANIM_COUNT) {
        return anim_meta_get_default_fps();
    }

    anim_config_t *cfg = &g_meta.animations[type];
    if (cfg->fps > 0) {
        return cfg->fps;
    }
    return g_meta.default_fps > 0 ? g_meta.default_fps : DEFAULT_FPS;
}

bool anim_meta_should_loop(emoji_anim_type_t type) {
    if (type < 0 || type >= EMOJI_ANIM_COUNT) {
        return DEFAULT_LOOP;
    }
    return g_meta.animations[type].loop;
}

int anim_meta_get_default_fps(void) {
    return g_meta.default_fps > 0 ? g_meta.default_fps : DEFAULT_FPS;
}
