/**
 * @file emoji_png.c
 * @brief Emoji PNG image loader implementation
 */

#include "emoji_png.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <inttypes.h>

#define TAG "EMOJI_PNG"

/* Storage mount point */
#define SPIFFS_MOUNT_POINT  "/spiffs"

/* Maximum file path length */
#define MAX_PATH_LEN        256

/* PNG header bytes */
static const uint8_t PNG_HEADER[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

/* Image descriptor arrays */
lv_img_dsc_t *g_emoji_images[EMOJI_ANIM_COUNT][MAX_EMOJI_IMAGES];
int g_emoji_counts[EMOJI_ANIM_COUNT];

static bool g_images_loaded = false;

bool emoji_images_loaded(void)
{
    return g_images_loaded;
}

/* Emoji type prefixes */
static const char *emoji_prefixes[] = {
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
        .max_files = 10,
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
        ESP_LOGI(TAG, "SPIFFS: total=%d bytes, used=%d bytes", total, used);
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

    /* Create image descriptor - same as factory_firmware */
    lv_img_dsc_t *img_dsc = (lv_img_dsc_t*)heap_caps_malloc(sizeof(lv_img_dsc_t), MALLOC_CAP_SPIRAM);
    if (img_dsc == NULL) {
        ESP_LOGE(TAG, "Failed to allocate image descriptor");
        heap_caps_free(data);
        return NULL;
    }

    /* CRITICAL: Use LV_IMG_CF_RAW_ALPHA for PNG data that needs decoding
     * This is the correct format for PNG images according to LVGL examples
     * LV_IMG_CF_TRUE_COLOR_ALPHA = already decoded pixels (wrong for PNG)
     * LV_IMG_CF_RAW_ALPHA = raw data with alpha (PNG decoder will handle it) */
    img_dsc->header.always_zero = 0;
    img_dsc->header.w = 412;
    img_dsc->header.h = 412;  /* Emoji images are 412x412 */
    img_dsc->header.cf = LV_IMG_CF_RAW_ALPHA;  /* Correct format for PNG with alpha */
    img_dsc->data_size = file_size;
    img_dsc->data = data;

    return img_dsc;
}

/* Simple insertion sort for small arrays - uses less stack than qsort */
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

static int load_emoji_type(emoji_anim_type_t type)
{
    const char *prefix = emoji_prefixes[type];
    int prefix_len = strlen(prefix);

    DIR *dir = opendir(SPIFFS_MOUNT_POINT);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open SPIFFS directory");
        return -1;
    }

    /* Allocate file array in PSRAM to save stack */
    sorted_file_t *files = (sorted_file_t*)heap_caps_malloc(
        MAX_EMOJI_IMAGES * sizeof(sorted_file_t), MALLOC_CAP_SPIRAM);
    if (files == NULL) {
        ESP_LOGE(TAG, "Failed to allocate file array");
        closedir(dir);
        return -1;
    }
    memset(files, 0, MAX_EMOJI_IMAGES * sizeof(sorted_file_t));

    int file_count = 0;
    struct dirent *ent;

    /* Collect matching files */
    while ((ent = readdir(dir)) != NULL && file_count < MAX_EMOJI_IMAGES) {
        if (strncmp(ent->d_name, prefix, prefix_len) == 0) {
            size_t len = strlen(ent->d_name);
            if (len > 4 && strcmp(ent->d_name + len - 4, ".png") == 0) {
                strncpy(files[file_count].name, ent->d_name, MAX_PATH_LEN - 1);
                files[file_count].name[MAX_PATH_LEN - 1] = '\0';
                files[file_count].index = extract_index(ent->d_name);
                file_count++;
            }
        }
    }
    closedir(dir);

    if (file_count == 0) {
        ESP_LOGW(TAG, "No images found for type: %s", prefix);
        heap_caps_free(files);
        return 0;
    }

    /* Sort files by index */
    sort_files_by_index(files, file_count);

    /* Load images in order */
    int loaded = 0;
    char filepath[MAX_PATH_LEN + 16];  /* /spiffs/ + filename + null */

    for (int i = 0; i < file_count; i++) {
        snprintf(filepath, sizeof(filepath), "%s/%s", SPIFFS_MOUNT_POINT, files[i].name);

        lv_img_dsc_t *img = load_png_image(filepath);
        if (img != NULL) {
            g_emoji_images[type][loaded] = img;
            loaded++;
            ESP_LOGI(TAG, "Loaded %s (%" PRIu32 " bytes)", files[i].name, img->data_size);
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

int emoji_load_all_images_with_cb(emoji_progress_cb_t cb)
{
    ESP_LOGI(TAG, "Loading all emoji images from SPIFFS...");

    memset(g_emoji_images, 0, sizeof(g_emoji_images));
    memset(g_emoji_counts, 0, sizeof(g_emoji_counts));

    int total = 0;
    for (int i = 0; i < EMOJI_ANIM_COUNT; i++) {
        int count = load_emoji_type((emoji_anim_type_t)i);
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
}

const char* emoji_type_name(emoji_anim_type_t type)
{
    if (type < 0 || type >= EMOJI_ANIM_COUNT) {
        return "unknown";
    }
    return emoji_names[type];
}
