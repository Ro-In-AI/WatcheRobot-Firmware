/**
 * @file anim_storage.c
 * @brief Animation catalog, warm-cache, and hot-cache implementation.
 */

#include "anim_storage.h"
#include "anim_meta.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define TAG "ANIM_STORAGE"

#define SPIFFS_MOUNT_POINT "/spiffs"
#define ANIM_DIR "/spiffs/anim"
#define ANIM_MANIFEST_MAGIC 0x4D494E41UL /* "ANIM" */
#define ANIM_MANIFEST_VERSION 1U

typedef struct {
    char name[ANIM_MAX_PATH_LEN];
    int index;
} sorted_file_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t type_count;
} anim_manifest_header_t;

typedef struct __attribute__((packed)) {
    uint16_t type_id;
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    uint16_t frame_count;
    uint8_t loop;
    uint8_t reserved[3];
    char name[24];
    char first_frame_raw[ANIM_MAX_PATH_LEN];
    char frame_paths[MAX_EMOJI_IMAGES][ANIM_MAX_PATH_LEN];
} anim_manifest_entry_t;

static const uint8_t PNG_HEADER[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

#ifdef CONFIG_WATCHER_ANIM_DEBUG_PERF
#define ANIM_DEBUG_PERF_ENABLED 1
#else
#define ANIM_DEBUG_PERF_ENABLED 0
#endif

#define PNG_IHDR_SIZE 13U
#define PNG_COLOR_TYPE_GRAYSCALE 0U
#define PNG_COLOR_TYPE_RGB 2U
#define PNG_COLOR_TYPE_INDEXED 3U
#define PNG_COLOR_TYPE_GRAYSCALE_ALPHA 4U
#define PNG_COLOR_TYPE_RGBA 6U

#if LV_COLOR_DEPTH == 16
#define ANIM_HOT_OPAQUE_BPP 2U
#define ANIM_HOT_OPAQUE_MODE_NAME "opaque-16bpp"
#else
#define ANIM_HOT_OPAQUE_BPP (((LV_COLOR_DEPTH) + 7U) / 8U)
#define ANIM_HOT_OPAQUE_MODE_NAME "opaque-native"
#endif

#define ANIM_HOT_CONSERVATIVE_BPP 3U

static const char *emoji_names[EMOJI_ANIM_COUNT] = {
    "boot",
    "happy",
    "error",
    "bluetooth",
    "speaking",
    "listening",
    "processing",
    "standby",
    "thinking",
    "custom1",
    "custom2",
    "custom3",
};

lv_img_dsc_t *g_emoji_images[EMOJI_ANIM_COUNT][MAX_EMOJI_IMAGES];
int g_emoji_counts[EMOJI_ANIM_COUNT];

static bool g_spiffs_initialized = false;
static bool g_images_loaded = false;
static bool g_catalog_initialized = false;
static bool g_warm_init_attempted = false;
static anim_catalog_type_info_t g_catalog[EMOJI_ANIM_COUNT];
static anim_warm_frame_t g_warm_frames[EMOJI_ANIM_COUNT];
static size_t g_warm_bytes = 0;
static anim_type_cache_t g_active_cache = {0};
static anim_type_cache_t g_prepared_cache = {0};
static SemaphoreHandle_t g_hot_cache_mutex = NULL;

static bool hot_cache_lock(void) {
    if (g_hot_cache_mutex == NULL) {
        g_hot_cache_mutex = xSemaphoreCreateMutex();
        if (g_hot_cache_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create hot cache mutex");
            return false;
        }
    }

    if (xSemaphoreTake(g_hot_cache_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to lock hot cache mutex");
        return false;
    }
    return true;
}

static void hot_cache_unlock(void) {
    if (g_hot_cache_mutex != NULL) {
        xSemaphoreGive(g_hot_cache_mutex);
    }
}

static uint32_t read_be32(const uint8_t *data) {
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static bool skip_file_bytes(FILE *f, size_t bytes) {
    return fseek(f, (long)bytes, SEEK_CUR) == 0;
}

static bool inspect_png_budget_class(const char *filepath, uint16_t *out_width, uint16_t *out_height, bool *out_use_conservative) {
    FILE *f = fopen(filepath, "rb");
    if (f == NULL) {
        return false;
    }

    uint8_t header[sizeof(PNG_HEADER)] = {0};
    if (fread(header, 1, sizeof(header), f) != sizeof(header) || memcmp(header, PNG_HEADER, sizeof(PNG_HEADER)) != 0) {
        fclose(f);
        return false;
    }

    bool seen_ihdr = false;
    bool use_conservative = true;
    uint16_t width = 0;
    uint16_t height = 0;

    for (;;) {
        uint8_t chunk_header[8] = {0};
        if (fread(chunk_header, 1, sizeof(chunk_header), f) != sizeof(chunk_header)) {
            break;
        }

        uint32_t chunk_len = read_be32(chunk_header);
        const uint8_t *chunk_type = &chunk_header[4];

        if (memcmp(chunk_type, "IHDR", 4) == 0) {
            if (chunk_len != PNG_IHDR_SIZE) {
                break;
            }

            uint8_t ihdr[PNG_IHDR_SIZE] = {0};
            if (fread(ihdr, 1, sizeof(ihdr), f) != sizeof(ihdr)) {
                break;
            }

            width = (uint16_t)read_be32(ihdr);
            height = (uint16_t)read_be32(&ihdr[4]);
            uint8_t color_type = ihdr[9];
            seen_ihdr = true;

            if (width == 0 || height == 0) {
                break;
            }

            switch (color_type) {
                case PNG_COLOR_TYPE_GRAYSCALE:
                case PNG_COLOR_TYPE_RGB:
                case PNG_COLOR_TYPE_INDEXED:
                    use_conservative = false;
                    break;
                case PNG_COLOR_TYPE_GRAYSCALE_ALPHA:
                case PNG_COLOR_TYPE_RGBA:
                    use_conservative = true;
                    break;
                default:
                    use_conservative = true;
                    break;
            }
        } else if (memcmp(chunk_type, "tRNS", 4) == 0) {
            use_conservative = true;
            if (!skip_file_bytes(f, chunk_len)) {
                break;
            }
        } else {
            if (!skip_file_bytes(f, chunk_len)) {
                break;
            }
        }

        if (!skip_file_bytes(f, 4U)) {
            break;
        }

        if (memcmp(chunk_type, "IDAT", 4) == 0 || memcmp(chunk_type, "IEND", 4) == 0) {
            if (seen_ihdr) {
                if (out_width != NULL) {
                    *out_width = width;
                }
                if (out_height != NULL) {
                    *out_height = height;
                }
                if (out_use_conservative != NULL) {
                    *out_use_conservative = use_conservative;
                }
                fclose(f);
                return true;
            }
            break;
        }
    }

    fclose(f);
    return false;
}

static size_t anim_hot_estimate_frame_bytes(uint16_t width, uint16_t height, bool use_conservative) {
    if (width == 0 || height == 0) {
        return 0;
    }

    const size_t bytes_per_pixel = use_conservative ? ANIM_HOT_CONSERVATIVE_BPP : (size_t)ANIM_HOT_OPAQUE_BPP;
    return (size_t)width * (size_t)height * bytes_per_pixel;
}

static size_t anim_hot_metadata_overhead_bytes(int frame_count) {
    if (frame_count <= 0) {
        return sizeof(anim_type_cache_t);
    }

    return sizeof(anim_type_cache_t) + ((size_t)frame_count * sizeof(anim_cached_frame_t));
}

static size_t anim_hot_estimate_type_bytes(const anim_catalog_type_info_t *info, bool *out_used_conservative) {
    if (info == NULL || info->frame_count <= 0) {
        if (out_used_conservative != NULL) {
            *out_used_conservative = true;
        }
        return 0;
    }

    bool used_conservative = false;
    size_t total_bytes = anim_hot_metadata_overhead_bytes(info->frame_count);
    for (int i = 0; i < info->frame_count; ++i) {
        uint16_t frame_width = info->width;
        uint16_t frame_height = info->height;
        bool use_conservative = true;
        bool inspected = inspect_png_budget_class(info->frame_paths[i], &frame_width, &frame_height, &use_conservative);
        if (!inspected) {
            use_conservative = true;
        }

        size_t frame_bytes = anim_hot_estimate_frame_bytes(frame_width, frame_height, use_conservative);
        if (frame_bytes == 0) {
            if (out_used_conservative != NULL) {
                *out_used_conservative = true;
            }
            return 0;
        }

        total_bytes += frame_bytes;
        used_conservative = used_conservative || use_conservative;
    }

    if (out_used_conservative != NULL) {
        *out_used_conservative = used_conservative;
    }
    return total_bytes;
}

static const char *anim_hot_budget_mode_name(bool used_conservative) {
    return used_conservative ? "conservative-alpha/unknown" : ANIM_HOT_OPAQUE_MODE_NAME;
}

static void reset_catalog(void) {
    memset(g_catalog, 0, sizeof(g_catalog));
    for (int i = 0; i < EMOJI_ANIM_COUNT; ++i) {
        g_catalog[i].type = (emoji_anim_type_t)i;
        strncpy(g_catalog[i].name, emoji_names[i], sizeof(g_catalog[i].name) - 1);
        g_catalog[i].fps = (uint16_t)anim_meta_get_fps((emoji_anim_type_t)i);
        g_catalog[i].loop = anim_meta_should_loop((emoji_anim_type_t)i);
    }
}

static void normalize_spiffs_path(const char *src, char *dst, size_t dst_size) {
    if (dst == NULL || dst_size == 0) {
        return;
    }

    dst[0] = '\0';
    if (src == NULL || src[0] == '\0') {
        return;
    }

    if (strncmp(src, SPIFFS_MOUNT_POINT, strlen(SPIFFS_MOUNT_POINT)) == 0) {
        snprintf(dst, dst_size, "%s", src);
        return;
    }

    if (strncmp(src, "anim/", 5) == 0) {
        snprintf(dst, dst_size, "%s/%s", SPIFFS_MOUNT_POINT, src);
        return;
    }

    if (strchr(src, '/') != NULL) {
        snprintf(dst, dst_size, "%s/%s", SPIFFS_MOUNT_POINT, src);
        return;
    }

    snprintf(dst, dst_size, "%s/%s", SPIFFS_MOUNT_POINT, src);
}

static void default_first_frame_path(emoji_anim_type_t type, char *dst, size_t dst_size) {
    snprintf(dst, dst_size, "%s/%s_first.raw565", ANIM_DIR, emoji_names[type]);
}

int emoji_spiffs_init(void) {
    if (g_spiffs_initialized) {
        return 0;
    }

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

    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS: total=%u bytes, used=%u bytes", (unsigned)total, (unsigned)used);
    }

    g_spiffs_initialized = true;
    return 0;
}

static int parse_frame_index_for_type(const char *filename, const char *prefix) {
    if (filename == NULL || prefix == NULL) {
        return -1;
    }

    const size_t prefix_len = strlen(prefix);
    if (strncmp(filename, prefix, prefix_len) != 0) {
        return -1;
    }

    const char *cursor = filename + prefix_len;
    if (*cursor == '_' || *cursor == '-') {
        ++cursor;
    }

    if (*cursor < '0' || *cursor > '9') {
        return -1;
    }

    char *end = NULL;
    long frame_index = strtol(cursor, &end, 10);
    if (end == cursor || frame_index <= 0 || frame_index > INT32_MAX) {
        return -1;
    }

    if (end == NULL || strcmp(end, ".png") != 0) {
        return -1;
    }

    return (int)frame_index;
}

static void sort_files_by_index(sorted_file_t *files, int count) {
    for (int i = 1; i < count; ++i) {
        sorted_file_t key = files[i];
        int j = i - 1;
        while (j >= 0 && files[j].index > key.index) {
            files[j + 1] = files[j];
            --j;
        }
        files[j + 1] = key;
    }
}

static int find_animation_files(const char *prefix, sorted_file_t *files, int max_files) {
    const char *dirs[] = {ANIM_DIR, SPIFFS_MOUNT_POINT};
    int file_count = 0;

    for (int d = 0; d < 2 && file_count == 0; ++d) {
        DIR *dir = opendir(dirs[d]);
        if (dir == NULL) {
            continue;
        }

        struct dirent *ent = NULL;
        while ((ent = readdir(dir)) != NULL && file_count < max_files) {
            int frame_index = parse_frame_index_for_type(ent->d_name, prefix);
            if (frame_index < 0) {
                continue;
            }

            size_t dir_len = strlen(dirs[d]);
            size_t name_len = strlen(ent->d_name);
            if (dir_len + 1 + name_len >= sizeof(files[file_count].name)) {
                continue;
            }

            memcpy(files[file_count].name, dirs[d], dir_len);
            files[file_count].name[dir_len] = '/';
            memcpy(files[file_count].name + dir_len + 1, ent->d_name, name_len + 1);
            files[file_count].index = frame_index;
            ++file_count;
        }

        closedir(dir);
    }

    sort_files_by_index(files, file_count);
    return file_count;
}

static int find_animation_files_for_type(emoji_anim_type_t type, sorted_file_t *files, int max_files) {
    return find_animation_files(emoji_names[type], files, max_files);
}

static int load_manifest_from_path(const char *manifest_path) {
    FILE *f = fopen(manifest_path, "rb");
    if (f == NULL) {
        return -1;
    }

    anim_manifest_header_t header = {0};
    if (fread(&header, sizeof(header), 1, f) != 1) {
        fclose(f);
        return -1;
    }

    if (header.magic != ANIM_MANIFEST_MAGIC || header.version != ANIM_MANIFEST_VERSION) {
        ESP_LOGW(TAG, "Invalid manifest header at %s", manifest_path);
        fclose(f);
        return -1;
    }

    int loaded = 0;
    for (uint16_t i = 0; i < header.type_count; ++i) {
        anim_manifest_entry_t entry = {0};
        if (fread(&entry, sizeof(entry), 1, f) != 1) {
            break;
        }

        if (entry.type_id >= EMOJI_ANIM_COUNT || entry.frame_count == 0) {
            continue;
        }

        anim_catalog_type_info_t *info = &g_catalog[entry.type_id];
        info->available = true;
        info->width = entry.width;
        info->height = entry.height;
        info->fps = entry.fps > 0 ? entry.fps : (uint16_t)anim_meta_get_fps((emoji_anim_type_t)entry.type_id);
        info->loop = entry.loop != 0;
        info->frame_count = entry.frame_count > MAX_EMOJI_IMAGES ? MAX_EMOJI_IMAGES : entry.frame_count;
        if (entry.name[0] != '\0') {
            strncpy(info->name, entry.name, sizeof(info->name) - 1);
        }

        normalize_spiffs_path(entry.first_frame_raw, info->first_frame_raw, sizeof(info->first_frame_raw));
        for (int frame = 0; frame < info->frame_count; ++frame) {
            normalize_spiffs_path(entry.frame_paths[frame], info->frame_paths[frame], sizeof(info->frame_paths[frame]));
        }

        ++loaded;
    }

    fclose(f);
    if (loaded > 0) {
        ESP_LOGI(TAG, "Loaded animation manifest from %s (%d types)", manifest_path, loaded);
        return 0;
    }

    return -1;
}

static int scan_catalog_from_fs(void) {
    int loaded = 0;

    for (int type = 0; type < EMOJI_ANIM_COUNT; ++type) {
        anim_catalog_type_info_t *info = &g_catalog[type];
        default_first_frame_path((emoji_anim_type_t)type, info->first_frame_raw, sizeof(info->first_frame_raw));

        sorted_file_t files[MAX_EMOJI_IMAGES] = {0};
        int count = find_animation_files_for_type((emoji_anim_type_t)type, files, MAX_EMOJI_IMAGES);
        if (count <= 0) {
            continue;
        }

        info->available = true;
        info->frame_count = (uint16_t)count;
        info->fps = (uint16_t)anim_meta_get_fps((emoji_anim_type_t)type);
        info->loop = anim_meta_should_loop((emoji_anim_type_t)type);
        for (int i = 0; i < count; ++i) {
            snprintf(info->frame_paths[i], sizeof(info->frame_paths[i]), "%s", files[i].name);
        }
        ++loaded;
    }

    if (loaded > 0) {
        ESP_LOGI(TAG, "Scanned animation catalog from SPIFFS (%d types)", loaded);
        return 0;
    }

    return -1;
}

static lv_img_dsc_t *load_png_descriptor(const char *filepath, uint16_t width, uint16_t height) {
    FILE *f = fopen(filepath, "rb");
    if (f == NULL) {
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size <= 0) {
        fclose(f);
        return NULL;
    }

    uint8_t header[sizeof(PNG_HEADER)] = {0};
    if (fread(header, 1, sizeof(header), f) != sizeof(header) || memcmp(header, PNG_HEADER, sizeof(PNG_HEADER)) != 0) {
        fclose(f);
        return NULL;
    }

    uint8_t *data = (uint8_t *)heap_caps_malloc((size_t)file_size, MALLOC_CAP_SPIRAM);
    if (data == NULL) {
        fclose(f);
        return NULL;
    }

    fseek(f, 0, SEEK_SET);
    if (fread(data, 1, (size_t)file_size, f) != (size_t)file_size) {
        heap_caps_free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);

    lv_img_dsc_t *img_dsc = (lv_img_dsc_t *)heap_caps_calloc(1, sizeof(lv_img_dsc_t), MALLOC_CAP_SPIRAM);
    if (img_dsc == NULL) {
        heap_caps_free(data);
        return NULL;
    }

    img_dsc->header.always_zero = 0;
    img_dsc->header.w = width;
    img_dsc->header.h = height;
    img_dsc->header.cf = LV_IMG_CF_RAW_ALPHA;
    img_dsc->data_size = (uint32_t)file_size;
    img_dsc->data = data;
    return img_dsc;
}

static uint8_t *decode_png_to_native_image(const char *filepath,
                                           int *width,
                                           int *height,
                                           size_t *data_size,
                                           lv_img_cf_t *color_format) {
    FILE *f = fopen(filepath, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "Failed to open PNG: %s", filepath);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size <= 0) {
        fclose(f);
        return NULL;
    }

    uint8_t *png_data = (uint8_t *)heap_caps_malloc((size_t)file_size, MALLOC_CAP_SPIRAM);
    if (png_data == NULL) {
        fclose(f);
        return NULL;
    }

    if (fread(png_data, 1, (size_t)file_size, f) != (size_t)file_size) {
        heap_caps_free(png_data);
        fclose(f);
        return NULL;
    }
    fclose(f);

    lv_img_dsc_t img_dsc = {0};
    img_dsc.data = png_data;
    img_dsc.data_size = (uint32_t)file_size;

    lv_img_decoder_dsc_t decoder_dsc;
    memset(&decoder_dsc, 0, sizeof(decoder_dsc));

    lvgl_port_lock(0);
    lv_res_t res = lv_img_decoder_open(&decoder_dsc, &img_dsc, lv_color_white(), 0);
    if (res != LV_RES_OK || decoder_dsc.img_data == NULL) {
        lvgl_port_unlock();
        heap_caps_free(png_data);
        return NULL;
    }

    size_t bytes_per_pixel = (size_t)(lv_img_cf_get_px_size(decoder_dsc.header.cf) >> 3);
    size_t decoded_size = (size_t)decoder_dsc.header.w * decoder_dsc.header.h * bytes_per_pixel;
    if (bytes_per_pixel == 0 || decoded_size == 0) {
        lv_img_decoder_close(&decoder_dsc);
        lvgl_port_unlock();
        heap_caps_free(png_data);
        return NULL;
    }

    uint8_t *decoded_data = (uint8_t *)heap_caps_malloc(decoded_size, MALLOC_CAP_SPIRAM);
    if (decoded_data == NULL) {
        lv_img_decoder_close(&decoder_dsc);
        lvgl_port_unlock();
        heap_caps_free(png_data);
        return NULL;
    }

    memcpy(decoded_data, decoder_dsc.img_data, decoded_size);
    *width = decoder_dsc.header.w;
    *height = decoder_dsc.header.h;
    *data_size = decoded_size;
    *color_format = decoder_dsc.header.cf;

    lv_img_decoder_close(&decoder_dsc);
    lvgl_port_unlock();
    heap_caps_free(png_data);

    return decoded_data;
}

static int legacy_load_type(emoji_anim_type_t type) {
    if (type < 0 || type >= EMOJI_ANIM_COUNT) {
        return -1;
    }

    const anim_catalog_type_info_t *info = anim_catalog_get_type_info(type);
    if (info == NULL || !info->available) {
        return 0;
    }

    int loaded = 0;
    for (int i = 0; i < info->frame_count; ++i) {
        lv_img_dsc_t *img = load_png_descriptor(info->frame_paths[i], info->width, info->height);
        if (img == NULL) {
            continue;
        }
        g_emoji_images[type][loaded] = img;
        ++loaded;
    }

    g_emoji_counts[type] = loaded;
    if (loaded > 0) {
        g_images_loaded = true;
    }
    return loaded;
}

static void free_legacy_type(emoji_anim_type_t type) {
    if (type < 0 || type >= EMOJI_ANIM_COUNT) {
        return;
    }

    for (int i = 0; i < g_emoji_counts[type]; ++i) {
        if (g_emoji_images[type][i] != NULL) {
            if (g_emoji_images[type][i]->data != NULL) {
                heap_caps_free((void *)g_emoji_images[type][i]->data);
            }
            heap_caps_free(g_emoji_images[type][i]);
            g_emoji_images[type][i] = NULL;
        }
    }
    g_emoji_counts[type] = 0;
}

static void free_warm_frame(anim_warm_frame_t *frame) {
    if (frame->img_data != NULL) {
        heap_caps_free(frame->img_data);
    }
    memset(frame, 0, sizeof(*frame));
}

#if LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP
static void swap_rgb565_bytes_in_place(uint8_t *data, size_t data_size) {
    if (data == NULL || (data_size & 0x1U) != 0U) {
        return;
    }

    for (size_t i = 0; i < data_size; i += 2U) {
        uint8_t tmp = data[i];
        data[i] = data[i + 1U];
        data[i + 1U] = tmp;
    }
}
#endif

static int load_raw565_frame(const anim_catalog_type_info_t *info, anim_warm_frame_t *frame) {
    FILE *f = fopen(info->first_frame_raw, "rb");
    if (f == NULL) {
        return -1;
    }

    size_t expected_size = (size_t)info->width * info->height * 2U;
    uint8_t *data = (uint8_t *)heap_caps_malloc(expected_size, MALLOC_CAP_SPIRAM);
    if (data == NULL) {
        fclose(f);
        return -1;
    }

    size_t read_size = fread(data, 1, expected_size, f);
    fclose(f);
    if (read_size != expected_size) {
        heap_caps_free(data);
        return -1;
    }

#if LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP
    /* Raw previews are stored as plain RGB565; swap to LVGL native 16-bit order on load. */
    swap_rgb565_bytes_in_place(data, expected_size);
#endif

    frame->loaded = true;
    frame->type = info->type;
    frame->width = info->width;
    frame->height = info->height;
    frame->data_size = expected_size;
    frame->color_format = LV_IMG_CF_TRUE_COLOR;
    frame->img_data = data;
    frame->img_dsc.header.always_zero = 0;
    frame->img_dsc.header.w = info->width;
    frame->img_dsc.header.h = info->height;
    frame->img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    frame->img_dsc.data_size = (uint32_t)expected_size;
    frame->img_dsc.data = data;
    return 0;
}

static int load_warm_frame(emoji_anim_type_t type) {
    if (type < 0 || type >= EMOJI_ANIM_COUNT) {
        return -1;
    }
    if (g_warm_frames[type].loaded) {
        return 0;
    }

    const anim_catalog_type_info_t *info = anim_catalog_get_type_info(type);
    if (info == NULL || !info->available || info->frame_count <= 0) {
        return -1;
    }

    anim_warm_frame_t frame = {0};
    int rc = -1;
    if (info->width > 0 && info->height > 0 && info->first_frame_raw[0] != '\0') {
        rc = load_raw565_frame(info, &frame);
    }

    if (rc != 0) {
        int width = 0;
        int height = 0;
        size_t data_size = 0;
        lv_img_cf_t color_format = LV_IMG_CF_UNKNOWN;
        uint8_t *decoded = decode_png_to_native_image(info->frame_paths[0], &width, &height, &data_size, &color_format);
        if (decoded == NULL) {
            return -1;
        }

        frame.loaded = true;
        frame.type = type;
        frame.width = width;
        frame.height = height;
        frame.data_size = data_size;
        frame.color_format = color_format;
        frame.img_data = decoded;
        frame.img_dsc.header.always_zero = 0;
        frame.img_dsc.header.w = width;
        frame.img_dsc.header.h = height;
        frame.img_dsc.header.cf = color_format;
        frame.img_dsc.data_size = (uint32_t)data_size;
        frame.img_dsc.data = decoded;
    }

    if (g_warm_bytes + frame.data_size > WATCHER_ANIM_WARM_BUDGET_BYTES) {
        free_warm_frame(&frame);
        ESP_LOGW(TAG, "Warm cache budget exceeded while loading %s", emoji_type_name(type));
        return -1;
    }

    g_warm_frames[type] = frame;
    g_warm_bytes += frame.data_size;
    return 0;
}

static void free_hot_cache(anim_type_cache_t *cache) {
    if (cache->frames != NULL) {
        for (int i = 0; i < cache->frame_count; ++i) {
            if (cache->frames[i].img_data != NULL) {
                heap_caps_free(cache->frames[i].img_data);
                cache->frames[i].img_data = NULL;
            }
        }
        heap_caps_free(cache->frames);
    }
    memset(cache, 0, sizeof(*cache));
}

bool anim_hot_can_build_type(emoji_anim_type_t type, anim_hot_build_budget_t *out_budget) {
    anim_hot_build_budget_t budget = {0};
    const anim_catalog_type_info_t *info = NULL;
    bool used_conservative = true;

    if (!anim_catalog_has_type(type)) {
        if (out_budget != NULL) {
            *out_budget = budget;
        }
        return false;
    }

    info = anim_catalog_get_type_info(type);
    if (info == NULL) {
        if (out_budget != NULL) {
            *out_budget = budget;
        }
        return false;
    }

    budget.estimated_bytes = anim_hot_estimate_type_bytes(info, &used_conservative);
    budget.free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    if (!hot_cache_lock()) {
        if (out_budget != NULL) {
            *out_budget = budget;
        }
        return false;
    }
    budget.active_bytes = g_active_cache.total_bytes;
    hot_cache_unlock();

    budget.can_build = budget.estimated_bytes > 0 &&
                       budget.estimated_bytes + budget.active_bytes <= WATCHER_ANIM_HOT_BUDGET_BYTES &&
                       budget.free_spiram > budget.estimated_bytes + WATCHER_ANIM_SAFETY_MARGIN_BYTES;

    if (budget.estimated_bytes > 0) {
        if (!budget.can_build) {
            ESP_LOGW(TAG,
                     "Hot cache precheck rejected for %s "
                     "(estimated=%u KB active=%u KB free=%u KB safety=%u KB hot_budget=%u KB estimate_mode=%s)",
                     emoji_type_name(type),
                     (unsigned)(budget.estimated_bytes / 1024U),
                     (unsigned)(budget.active_bytes / 1024U),
                     (unsigned)(budget.free_spiram / 1024U),
                     (unsigned)(WATCHER_ANIM_SAFETY_MARGIN_BYTES / 1024U),
                     (unsigned)(WATCHER_ANIM_HOT_BUDGET_BYTES / 1024U),
                     anim_hot_budget_mode_name(used_conservative));
        } else if (ANIM_DEBUG_PERF_ENABLED) {
            ESP_LOGI(TAG,
                     "Hot cache precheck accepted for %s "
                     "(estimated=%u KB active=%u KB free=%u KB safety=%u KB hot_budget=%u KB estimate_mode=%s)",
                     emoji_type_name(type),
                     (unsigned)(budget.estimated_bytes / 1024U),
                     (unsigned)(budget.active_bytes / 1024U),
                     (unsigned)(budget.free_spiram / 1024U),
                     (unsigned)(WATCHER_ANIM_SAFETY_MARGIN_BYTES / 1024U),
                     (unsigned)(WATCHER_ANIM_HOT_BUDGET_BYTES / 1024U),
                     anim_hot_budget_mode_name(used_conservative));
        }
    }

    if (out_budget != NULL) {
        *out_budget = budget;
    }
    return budget.can_build;
}

int anim_catalog_init(void) {
    if (g_catalog_initialized) {
        return 0;
    }
    if (emoji_spiffs_init() != 0) {
        return -1;
    }

    anim_meta_init();
    reset_catalog();

    if (load_manifest_from_path(ANIM_MANIFEST_PATH) != 0 && load_manifest_from_path(ANIM_MANIFEST_FALLBACK_PATH) != 0 &&
        scan_catalog_from_fs() != 0) {
        ESP_LOGW(TAG, "No animation catalog available");
        return -1;
    }

    g_catalog_initialized = true;
    return 0;
}

const anim_catalog_type_info_t *anim_catalog_get_type_info(emoji_anim_type_t type) {
    if (!g_catalog_initialized) {
        anim_catalog_init();
    }
    if (type < 0 || type >= EMOJI_ANIM_COUNT) {
        return NULL;
    }
    return &g_catalog[type];
}

bool anim_catalog_has_type(emoji_anim_type_t type) {
    const anim_catalog_type_info_t *info = anim_catalog_get_type_info(type);
    return info != NULL && info->available && info->frame_count > 0;
}

int anim_warm_init_all_types(void) {
    if (anim_catalog_init() != 0) {
        return -1;
    }

    if (g_warm_init_attempted) {
        int loaded = 0;
        for (int type = 0; type < EMOJI_ANIM_COUNT; ++type) {
            if (g_warm_frames[type].loaded) {
                ++loaded;
            }
        }
        ESP_LOGI(TAG, "Warm cache already initialized: %d types, %u KB", loaded, (unsigned)(g_warm_bytes / 1024U));
        return loaded > 0 ? 0 : -1;
    }

    g_warm_init_attempted = true;

    int loaded = 0;
    for (int type = 0; type < EMOJI_ANIM_COUNT; ++type) {
        if (type == EMOJI_ANIM_BOOT) {
            continue;
        }
        if (!anim_catalog_has_type((emoji_anim_type_t)type)) {
            continue;
        }
        if (load_warm_frame((emoji_anim_type_t)type) == 0) {
            ++loaded;
        }
    }

    ESP_LOGI(TAG, "Warm cache initialized: %d types, %u KB", loaded, (unsigned)(g_warm_bytes / 1024U));
    return loaded > 0 ? 0 : -1;
}

const lv_img_dsc_t *anim_warm_get_first_frame(emoji_anim_type_t type) {
    if (type < 0 || type >= EMOJI_ANIM_COUNT) {
        return NULL;
    }
    if (!g_warm_frames[type].loaded) {
        if (load_warm_frame(type) != 0) {
            return NULL;
        }
    }
    return &g_warm_frames[type].img_dsc;
}

int anim_hot_build_type(emoji_anim_type_t type, uint32_t generation_id) {
    anim_hot_build_budget_t budget = {0};

    if (!anim_catalog_has_type(type)) {
        return -1;
    }

    const anim_catalog_type_info_t *info = anim_catalog_get_type_info(type);
    if (!hot_cache_lock()) {
        return -1;
    }
    free_hot_cache(&g_prepared_cache);
    hot_cache_unlock();

    if (!anim_hot_can_build_type(type, &budget)) {
        ESP_LOGW(TAG, "Insufficient PSRAM headroom for %s hot cache", emoji_type_name(type));
        return -1;
    }
    anim_type_cache_t cache = {0};
    cache.frames = (anim_cached_frame_t *)heap_caps_calloc((size_t)info->frame_count, sizeof(anim_cached_frame_t), MALLOC_CAP_SPIRAM);
    if (cache.frames == NULL) {
        return -1;
    }

    cache.type = type;
    cache.frame_count = info->frame_count;
    cache.generation_id = generation_id;

    for (int i = 0; i < info->frame_count; ++i) {
        int width = 0;
        int height = 0;
        size_t data_size = 0;
        lv_img_cf_t color_format = LV_IMG_CF_UNKNOWN;
        uint8_t *decoded = decode_png_to_native_image(info->frame_paths[i], &width, &height, &data_size, &color_format);
        if (decoded == NULL) {
            free_hot_cache(&cache);
            return -1;
        }

        cache.frames[i].img_data = decoded;
        cache.frames[i].data_size = data_size;
        cache.frames[i].width = width;
        cache.frames[i].height = height;
        cache.frames[i].color_format = color_format;
        cache.frames[i].img_dsc.header.always_zero = 0;
        cache.frames[i].img_dsc.header.w = width;
        cache.frames[i].img_dsc.header.h = height;
        cache.frames[i].img_dsc.header.cf = color_format;
        cache.frames[i].img_dsc.data_size = (uint32_t)data_size;
        cache.frames[i].img_dsc.data = decoded;
        cache.total_bytes += data_size;
    }

    if (cache.total_bytes + budget.active_bytes > WATCHER_ANIM_HOT_BUDGET_BYTES) {
        ESP_LOGW(TAG, "Hot cache budget exceeded for %s (%u KB)", emoji_type_name(type),
                 (unsigned)(cache.total_bytes / 1024U));
        free_hot_cache(&cache);
        return -1;
    }

    if (ANIM_DEBUG_PERF_ENABLED) {
        ESP_LOGI(TAG,
                 "Hot cache built for %s (estimated=%u KB actual=%u KB active=%u KB budget=%u KB)",
                 emoji_type_name(type),
                 (unsigned)(budget.estimated_bytes / 1024U),
                 (unsigned)(cache.total_bytes / 1024U),
                 (unsigned)(budget.active_bytes / 1024U),
                 (unsigned)(WATCHER_ANIM_HOT_BUDGET_BYTES / 1024U));
    }

    cache.is_loaded = true;
    if (!hot_cache_lock()) {
        free_hot_cache(&cache);
        return -1;
    }
    free_hot_cache(&g_prepared_cache);
    g_prepared_cache = cache;
    hot_cache_unlock();
    return 0;
}

int anim_hot_commit_prepared(emoji_anim_type_t type, uint32_t generation_id) {
    if (!hot_cache_lock()) {
        return -1;
    }
    if (!g_prepared_cache.is_loaded || g_prepared_cache.type != type || g_prepared_cache.generation_id != generation_id) {
        hot_cache_unlock();
        return -1;
    }

    free_hot_cache(&g_active_cache);
    g_active_cache = g_prepared_cache;
    memset(&g_prepared_cache, 0, sizeof(g_prepared_cache));
    hot_cache_unlock();
    return 0;
}

void anim_hot_discard_prepared(void) {
    if (!hot_cache_lock()) {
        return;
    }
    free_hot_cache(&g_prepared_cache);
    hot_cache_unlock();
}

bool anim_hot_is_active_type(emoji_anim_type_t type) {
    return g_active_cache.is_loaded && g_active_cache.type == type;
}

emoji_anim_type_t anim_hot_get_active_type(void) {
    return g_active_cache.is_loaded ? g_active_cache.type : EMOJI_ANIM_NONE;
}

void anim_hot_release_active(void) {
    if (!hot_cache_lock()) {
        return;
    }
    free_hot_cache(&g_active_cache);
    hot_cache_unlock();
}

int anim_hot_get_frame_count(void) {
    return g_active_cache.is_loaded ? g_active_cache.frame_count : 0;
}

anim_cached_frame_t *anim_hot_get_frame(emoji_anim_type_t type, int frame) {
    if (!anim_hot_is_active_type(type) || frame < 0 || frame >= g_active_cache.frame_count) {
        return NULL;
    }
    return &g_active_cache.frames[frame];
}

void anim_hot_free_all(void) {
    if (!hot_cache_lock()) {
        return;
    }
    free_hot_cache(&g_active_cache);
    free_hot_cache(&g_prepared_cache);
    hot_cache_unlock();
}

int emoji_load_all_images(void) {
    return emoji_load_all_images_with_cb(NULL);
}

int emoji_load_type(emoji_anim_type_t type) {
    if (anim_catalog_init() != 0) {
        return -1;
    }
    if (type < 0 || type >= EMOJI_ANIM_COUNT) {
        return -1;
    }
    if (g_emoji_counts[type] > 0 && g_emoji_images[type][0] != NULL) {
        return g_emoji_counts[type];
    }
    return legacy_load_type(type);
}

int emoji_load_all_images_with_cb(emoji_progress_cb_t cb) {
    if (anim_catalog_init() != 0) {
        return -1;
    }

    int total = 0;
    for (int type = 0; type < EMOJI_ANIM_COUNT; ++type) {
        if (!anim_catalog_has_type((emoji_anim_type_t)type)) {
            continue;
        }

        int count = emoji_load_type((emoji_anim_type_t)type);
        if (count > 0) {
            total += count;
        }
        if (cb != NULL) {
            cb((emoji_anim_type_t)type, type + 1, EMOJI_ANIM_COUNT);
        }
    }
    return total > 0 ? 0 : -1;
}

lv_img_dsc_t *emoji_get_image(emoji_anim_type_t type, int frame) {
    if (type < 0 || type >= EMOJI_ANIM_COUNT) {
        return NULL;
    }
    if (frame < 0) {
        return NULL;
    }

    if (frame == 0) {
        const lv_img_dsc_t *warm = anim_warm_get_first_frame(type);
        if (warm != NULL) {
            return (lv_img_dsc_t *)warm;
        }
    }

    if (frame < g_emoji_counts[type]) {
        return g_emoji_images[type][frame];
    }
    return NULL;
}

int emoji_get_frame_count(emoji_anim_type_t type) {
    if (type < 0 || type >= EMOJI_ANIM_COUNT) {
        return 0;
    }
    const anim_catalog_type_info_t *info = anim_catalog_get_type_info(type);
    if (info != NULL && info->available) {
        anim_config_t *cfg = anim_meta_get_config(type);
        if (cfg->frame_count > 0 && cfg->frame_count < info->frame_count) {
            return cfg->frame_count;
        }
        return info->frame_count;
    }
    return g_emoji_counts[type];
}

void emoji_free_all(void) {
    for (int type = 0; type < EMOJI_ANIM_COUNT; ++type) {
        free_legacy_type((emoji_anim_type_t)type);
    }
    g_images_loaded = false;
}

bool emoji_images_loaded(void) {
    return g_images_loaded;
}

const char *emoji_type_name(emoji_anim_type_t type) {
    if (type < 0 || type >= EMOJI_ANIM_COUNT) {
        return "unknown";
    }
    return emoji_names[type];
}
