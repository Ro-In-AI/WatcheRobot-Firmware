/**
 * @file anim_storage.c
 * @brief Emoji PNG image loader and RGB565 cache implementation
 */

#include "anim_storage.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "lvgl.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <inttypes.h>

#define TAG "ANIM_STORAGE"

/* Storage mount point */
#define SPIFFS_MOUNT_POINT  "/spiffs"

/* Animation directory */
#define ANIM_DIR            "/spiffs/anim"

/* Maximum file path length */
#define MAX_PATH_LEN        512

/* Image dimensions */
#define EMOJI_IMG_WIDTH     412
#define EMOJI_IMG_HEIGHT    412
/* PNG header bytes */
static const uint8_t PNG_HEADER[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

/* ------------------------------------------------------------------ */
/* Legacy PNG Storage                                                 */
/* ------------------------------------------------------------------ */

/* Image descriptor arrays */
lv_img_dsc_t *g_emoji_images[EMOJI_ANIM_COUNT][MAX_EMOJI_IMAGES];
int g_emoji_counts[EMOJI_ANIM_COUNT];

static bool g_images_loaded = false;

/* ------------------------------------------------------------------ */
/* RGB565 Cache                                                       */
/* ------------------------------------------------------------------ */

static anim_type_cache_t g_cache = {0};

bool emoji_images_loaded(void)
{
    return g_images_loaded;
}

/* Emoji type prefixes */
static const char *emoji_prefixes[] = {
    "boot",
    "greeting",
    "detecting",
    "detected",
    "speaking",
    "listening",
    "analyzing",
    "standby"
};

/* Emoji type names */
static const char *emoji_names[] = {
    "boot",
    "greeting",
    "detecting",
    "detected",
    "speaking",
    "listening",
    "analyzing",
    "standby"
};

/* File sorting structure */
typedef struct {
    char name[MAX_PATH_LEN];
    int index;
} sorted_file_t;

int emoji_spiffs_init(void)
{
    ESP_LOGI(TAG, "Initializing SPIFFS...");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_MOUNT_POINT,
        .partition_label = "storage",
        .max_files = 20,
        .format_if_mount_failed = false,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return -1;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info("storage", &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS: total=%u bytes, used=%u bytes", total, used);
    }

    return 0;
}

static int extract_index(const char *filename)
{
    /* Extract number from filename like "speaking1.png" or "greeting2.png" */
    const char *p = filename;
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            return atoi(p);
        }
        p++;
    }
    return 0;
}

static lv_img_dsc_t* load_png_image(const char *filepath)
{
    FILE *f = fopen(filepath, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "Failed to open file: %s", filepath);
        return NULL;
    }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) {
        ESP_LOGW(TAG, "Empty file: %s", filepath);
        fclose(f);
        return NULL;
    }

    /* Verify PNG header */
    uint8_t header[8];
    if (fread(header, 1, 8, f) != 8) {
        ESP_LOGW(TAG, "Failed to read header: %s", filepath);
        fclose(f);
        return NULL;
    }

    if (memcmp(header, PNG_HEADER, 8) != 0) {
        ESP_LOGW(TAG, "Not a valid PNG file: %s", filepath);
        fclose(f);
        return NULL;
    }

    /* Allocate buffer for PNG data (in PSRAM for large images) */
    uint8_t *data = (uint8_t*)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
    if (data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate %ld bytes for %s", file_size, filepath);
        fclose(f);
        return NULL;
    }

    /* Read entire file */
    fseek(f, 0, SEEK_SET);
    if (fread(data, 1, file_size, f) != (size_t)file_size) {
        ESP_LOGW(TAG, "Failed to read file: %s", filepath);
        heap_caps_free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);

    /* Create image descriptor */
    lv_img_dsc_t *img_dsc = (lv_img_dsc_t*)heap_caps_malloc(sizeof(lv_img_dsc_t), MALLOC_CAP_SPIRAM);
    if (img_dsc == NULL) {
        ESP_LOGE(TAG, "Failed to allocate image descriptor");
        heap_caps_free(data);
        return NULL;
    }

    img_dsc->header.always_zero = 0;
    img_dsc->header.w = EMOJI_IMG_WIDTH;
    img_dsc->header.h = EMOJI_IMG_HEIGHT;
    img_dsc->header.cf = LV_IMG_CF_RAW_ALPHA;
    img_dsc->data_size = file_size;
    img_dsc->data = data;

    return img_dsc;
}

/* Simple insertion sort for small arrays */
static void sort_files_by_index(sorted_file_t *files, int count)
{
    for (int i = 1; i < count; i++) {
        sorted_file_t key = files[i];
        int j = i - 1;
        while (j >= 0 && files[j].index > key.index) {
            files[j + 1] = files[j];
            j--;
        }
        files[j + 1] = key;
    }
}

static int find_animation_files(const char *prefix, sorted_file_t *files, int max_files)
{
    int prefix_len = strlen(prefix);
    int file_count = 0;

    /* Try anim directory first, then root */
    const char *dirs[] = {ANIM_DIR, SPIFFS_MOUNT_POINT};

    for (int d = 0; d < 2 && file_count == 0; d++) {
        DIR *dir = opendir(dirs[d]);
        if (dir == NULL) {
            continue;
        }

        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL && file_count < max_files) {
            if (strncmp(ent->d_name, prefix, prefix_len) == 0) {
                size_t len = strlen(ent->d_name);
                if (len > 4 && strcmp(ent->d_name + len - 4, ".png") == 0) {
                    /* Store full path with length check */
                    size_t dir_len = strlen(dirs[d]);
                    if (dir_len + 1 + len < MAX_PATH_LEN) {
                        snprintf(files[file_count].name, MAX_PATH_LEN, "%s/%s",
                                 dirs[d], ent->d_name);
                        files[file_count].index = extract_index(ent->d_name);
                        file_count++;
                    }
                }
            }
        }
        closedir(dir);
    }

    /* Sort by index */
    sort_files_by_index(files, file_count);

    return file_count;
}

static int load_emoji_type(emoji_anim_type_t type)
{
    const char *prefix = emoji_prefixes[type];

    /* Allocate file array in PSRAM */
    sorted_file_t *files = (sorted_file_t*)heap_caps_malloc(
        MAX_EMOJI_IMAGES * sizeof(sorted_file_t), MALLOC_CAP_SPIRAM);
    if (files == NULL) {
        ESP_LOGE(TAG, "Failed to allocate file array");
        return -1;
    }
    memset(files, 0, MAX_EMOJI_IMAGES * sizeof(sorted_file_t));

    int file_count = find_animation_files(prefix, files, MAX_EMOJI_IMAGES);

    if (file_count == 0) {
        ESP_LOGW(TAG, "No images found for type: %s", prefix);
        heap_caps_free(files);
        return 0;
    }

    /* Load images in order */
    int loaded = 0;

    for (int i = 0; i < file_count; i++) {
        lv_img_dsc_t *img = load_png_image(files[i].name);
        if (img != NULL) {
            g_emoji_images[type][loaded] = img;
            loaded++;
            ESP_LOGD(TAG, "Loaded %s (%" PRIu32 " bytes)", files[i].name, img->data_size);
        }
    }

    heap_caps_free(files);

    g_emoji_counts[type] = loaded;
    ESP_LOGI(TAG, "Loaded %d images for type: %s", loaded, prefix);

    return loaded;
}

int emoji_load_all_images(void)
{
    return emoji_load_all_images_with_cb(NULL);
}

int emoji_load_type(emoji_anim_type_t type)
{
    if (type < 0 || type >= EMOJI_ANIM_COUNT) {
        return -1;
    }

    if (g_emoji_counts[type] > 0 && g_emoji_images[type][0] != NULL) {
        return g_emoji_counts[type];
    }

    int count = load_emoji_type(type);
    if (count > 0) {
        g_images_loaded = true;
    }
    return count;
}

int emoji_load_all_images_with_cb(emoji_progress_cb_t cb)
{
    ESP_LOGI(TAG, "Loading all emoji images from SPIFFS...");

    int total = 0;
    for (int i = 0; i < EMOJI_ANIM_COUNT; i++) {
        int count = g_emoji_counts[i];
        if (count <= 0 || g_emoji_images[i][0] == NULL) {
            count = load_emoji_type((emoji_anim_type_t)i);
        } else {
            ESP_LOGI(TAG, "Images already loaded for type: %s (%d)",
                     emoji_type_name((emoji_anim_type_t)i), count);
        }

        if (count < 0) {
            ESP_LOGW(TAG, "Failed to load type %d", i);
        } else {
            total += count;
        }
        if (cb) {
            cb((emoji_anim_type_t)i, i + 1, EMOJI_ANIM_COUNT);
        }
    }

    ESP_LOGI(TAG, "Total %d emoji images loaded", total);
    g_images_loaded = (total > 0);
    return total > 0 ? 0 : -1;
}

lv_img_dsc_t* emoji_get_image(emoji_anim_type_t type, int frame)
{
    if (type < 0 || type >= EMOJI_ANIM_COUNT) {
        return NULL;
    }
    if (frame < 0 || frame >= g_emoji_counts[type]) {
        return NULL;
    }
    return g_emoji_images[type][frame];
}

int emoji_get_frame_count(emoji_anim_type_t type)
{
    if (type < 0 || type >= EMOJI_ANIM_COUNT) {
        return 0;
    }
    return g_emoji_counts[type];
}

void emoji_free_all(void)
{
    for (int t = 0; t < EMOJI_ANIM_COUNT; t++) {
        for (int i = 0; i < g_emoji_counts[t]; i++) {
            if (g_emoji_images[t][i] != NULL) {
                if (g_emoji_images[t][i]->data != NULL) {
                    heap_caps_free((void*)g_emoji_images[t][i]->data);
                }
                heap_caps_free(g_emoji_images[t][i]);
                g_emoji_images[t][i] = NULL;
            }
        }
        g_emoji_counts[t] = 0;
    }
    g_images_loaded = false;
}

const char* emoji_type_name(emoji_anim_type_t type)
{
    if (type < 0 || type >= EMOJI_ANIM_COUNT) {
        return "unknown";
    }
    return emoji_names[type];
}

/* ------------------------------------------------------------------ */
/* RGB565 Cache Implementation                                        */
/* ------------------------------------------------------------------ */

static bool g_cache_initialized = false;

int anim_cache_init(void)
{
    /* Don't re-initialize and clear existing cache */
    if (g_cache_initialized) {
        ESP_LOGD(TAG, "RGB565 cache already initialized, skipping");
        return 0;
    }

    memset(&g_cache, 0, sizeof(g_cache));
    g_cache_initialized = true;
    ESP_LOGI(TAG, "RGB565 cache initialized");
    return 0;
}

static void free_cached_type(void)
{
    if (g_cache.frames != NULL) {
        for (int i = 0; i < g_cache.frame_count; i++) {
            if (g_cache.frames[i].img_data != NULL) {
                heap_caps_free(g_cache.frames[i].img_data);
                g_cache.frames[i].img_data = NULL;
            }
            memset(&g_cache.frames[i].img_dsc, 0, sizeof(g_cache.frames[i].img_dsc));
        }
        heap_caps_free(g_cache.frames);
        g_cache.frames = NULL;
    }
    g_cache.frame_count = 0;
    g_cache.is_loaded = false;
    g_cache.type = EMOJI_ANIM_NONE;
}

static uint8_t* decode_png_to_native_image(const char *filepath, int *width, int *height,
                                           size_t *data_size, lv_img_cf_t *color_format)
{
    FILE *f = fopen(filepath, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "Failed to open PNG: %s", filepath);
        return NULL;
    }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(f);
        return NULL;
    }

    /* Read entire PNG file into memory */
    uint8_t *png_data = (uint8_t*)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
    if (png_data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate %ld bytes for PNG", file_size);
        fclose(f);
        return NULL;
    }

    if (fread(png_data, 1, file_size, f) != (size_t)file_size) {
        heap_caps_free(png_data);
        fclose(f);
        return NULL;
    }
    fclose(f);

    /* Use LVGL's PNG decoder to get image info */
    lv_img_dsc_t img_dsc = {0};
    img_dsc.data_size = file_size;
    img_dsc.data = png_data;

    /* Decode using LVGL's built-in decoder (LVGL 8.x API) */
    lv_img_decoder_dsc_t decoder_dsc;
    lv_res_t res = lv_img_decoder_open(&decoder_dsc, &img_dsc, lv_color_white(), 0);

    if (res != LV_RES_OK || decoder_dsc.img_data == NULL) {
        ESP_LOGW(TAG, "LVGL decoder failed for %s", filepath);
        heap_caps_free(png_data);
        return NULL;
    }

    size_t bytes_per_pixel = lv_img_cf_get_px_size(decoder_dsc.header.cf) >> 3;
    size_t decoded_size = (size_t)decoder_dsc.header.w * decoder_dsc.header.h * bytes_per_pixel;
    if (bytes_per_pixel == 0 || decoded_size == 0) {
        ESP_LOGW(TAG, "Skip cache for unsupported decoded color format: %d (%s)",
                 decoder_dsc.header.cf, filepath);
        lv_img_decoder_close(&decoder_dsc);
        heap_caps_free(png_data);
        return NULL;
    }

    /* Allocate native decoded image buffer */
    uint8_t *decoded_data = (uint8_t*)heap_caps_malloc(decoded_size, MALLOC_CAP_SPIRAM);
    if (decoded_data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate decoded image buffer (%u bytes)",
                 (unsigned)decoded_size);
        lv_img_decoder_close(&decoder_dsc);
        heap_caps_free(png_data);
        return NULL;
    }

    /* Copy decoded data in LVGL's native color format */
    memcpy(decoded_data, decoder_dsc.img_data, decoded_size);

    *width = decoder_dsc.header.w;
    *height = decoder_dsc.header.h;
    *data_size = decoded_size;
    *color_format = decoder_dsc.header.cf;

    lv_img_decoder_close(&decoder_dsc);
    heap_caps_free(png_data);

    return decoded_data;
}

int anim_cache_load_type(emoji_anim_type_t type)
{
    if (type < 0 || type >= EMOJI_ANIM_COUNT) {
        ESP_LOGE(TAG, "Invalid animation type: %d", type);
        return -1;
    }

    /* If already cached, nothing to do */
    if (g_cache.is_loaded && g_cache.type == type) {
        ESP_LOGD(TAG, "Type %s already cached", emoji_type_name(type));
        return 0;
    }

    /* Free previous cache */
    free_cached_type();

    const char *prefix = emoji_prefixes[type];

    /* Find files */
    sorted_file_t *files = (sorted_file_t*)heap_caps_malloc(
        MAX_EMOJI_IMAGES * sizeof(sorted_file_t), MALLOC_CAP_SPIRAM);
    if (files == NULL) {
        ESP_LOGE(TAG, "Failed to allocate file array");
        return -1;
    }
    memset(files, 0, MAX_EMOJI_IMAGES * sizeof(sorted_file_t));

    int file_count = find_animation_files(prefix, files, MAX_EMOJI_IMAGES);

    if (file_count == 0) {
        ESP_LOGW(TAG, "No images found for type: %s", prefix);
        heap_caps_free(files);
        return -1;
    }

    /* Allocate frame array */
    g_cache.frames = (anim_cached_frame_t*)heap_caps_malloc(
        file_count * sizeof(anim_cached_frame_t), MALLOC_CAP_SPIRAM);
    if (g_cache.frames == NULL) {
        ESP_LOGE(TAG, "Failed to allocate frame array");
        heap_caps_free(files);
        return -1;
    }
    memset(g_cache.frames, 0, file_count * sizeof(anim_cached_frame_t));

    /* Decode each frame */
    int loaded = 0;
    size_t total_bytes = 0;
    for (int i = 0; i < file_count; i++) {
        int w, h;
        size_t data_size = 0;
        lv_img_cf_t color_format = LV_IMG_CF_UNKNOWN;
        uint8_t *decoded = decode_png_to_native_image(files[i].name, &w, &h,
                                                      &data_size, &color_format);
        if (decoded != NULL) {
            g_cache.frames[loaded].img_data = decoded;
            g_cache.frames[loaded].data_size = data_size;
            g_cache.frames[loaded].width = w;
            g_cache.frames[loaded].height = h;
            g_cache.frames[loaded].img_dsc.header.always_zero = 0;
            g_cache.frames[loaded].img_dsc.header.w = w;
            g_cache.frames[loaded].img_dsc.header.h = h;
            g_cache.frames[loaded].img_dsc.header.cf = color_format;
            g_cache.frames[loaded].img_dsc.data_size = data_size;
            g_cache.frames[loaded].img_dsc.data = decoded;
            total_bytes += data_size;
            loaded++;
            ESP_LOGD(TAG, "Cached frame %d: %s", loaded, files[i].name);
        }
    }

    heap_caps_free(files);

    if (loaded == 0) {
        ESP_LOGE(TAG, "Failed to decode any frames for type: %s", prefix);
        heap_caps_free(g_cache.frames);
        g_cache.frames = NULL;
        return -1;
    }

    g_cache.frame_count = loaded;
    g_cache.is_loaded = true;
    g_cache.type = type;

    ESP_LOGI(TAG, "Cached %d frames for type: %s (~%u KB)",
             loaded, emoji_type_name(type), (unsigned)(total_bytes / 1024));

    return 0;
}

void anim_cache_unload_type(emoji_anim_type_t type)
{
    if (g_cache.type == type) {
        free_cached_type();
        ESP_LOGI(TAG, "Unloaded cache for type: %s", emoji_type_name(type));
    }
}

anim_cached_frame_t* anim_cache_get_frame(emoji_anim_type_t type, int frame)
{
    if (!g_cache.is_loaded || g_cache.type != type) {
        return NULL;
    }
    if (frame < 0 || frame >= g_cache.frame_count) {
        return NULL;
    }
    return &g_cache.frames[frame];
}

bool anim_cache_is_type_loaded(emoji_anim_type_t type)
{
    return g_cache.is_loaded && g_cache.type == type;
}

emoji_anim_type_t anim_cache_get_current_type(void)
{
    return g_cache.is_loaded ? g_cache.type : EMOJI_ANIM_NONE;
}

int anim_cache_get_frame_count(void)
{
    return g_cache.is_loaded ? g_cache.frame_count : 0;
}

void anim_cache_free_all(void)
{
    free_cached_type();
    ESP_LOGI(TAG, "Freed all RGB565 cache");
}
