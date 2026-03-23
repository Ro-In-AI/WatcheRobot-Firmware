/**
 * @file anim_storage.h
 * @brief Animation catalog, warm-cache, and hot-cache management.
 */

#ifndef ANIM_STORAGE_H
#define ANIM_STORAGE_H

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

/* Maximum frames per animation type (configurable via Kconfig) */
#ifdef CONFIG_WATCHER_ANIM_MAX_FRAMES_PER_TYPE
#define MAX_EMOJI_IMAGES CONFIG_WATCHER_ANIM_MAX_FRAMES_PER_TYPE
#else
#define MAX_EMOJI_IMAGES 24
#endif

#ifdef CONFIG_WATCHER_ANIM_WARM_BUDGET_KB
#define WATCHER_ANIM_WARM_BUDGET_BYTES ((size_t)CONFIG_WATCHER_ANIM_WARM_BUDGET_KB * 1024U)
#else
#define WATCHER_ANIM_WARM_BUDGET_BYTES (1024U * 1024U)
#endif

#ifdef CONFIG_WATCHER_ANIM_HOT_BUDGET_KB
#define WATCHER_ANIM_HOT_BUDGET_BYTES ((size_t)CONFIG_WATCHER_ANIM_HOT_BUDGET_KB * 1024U)
#else
#define WATCHER_ANIM_HOT_BUDGET_BYTES (4096U * 1024U)
#endif

#ifdef CONFIG_WATCHER_ANIM_SAFETY_MARGIN_KB
#define WATCHER_ANIM_SAFETY_MARGIN_BYTES ((size_t)CONFIG_WATCHER_ANIM_SAFETY_MARGIN_KB * 1024U)
#else
#define WATCHER_ANIM_SAFETY_MARGIN_BYTES (512U * 1024U)
#endif

#define ANIM_MAX_PATH_LEN 96
#define ANIM_MANIFEST_PATH "/spiffs/anim/anim_manifest.bin"
#define ANIM_MANIFEST_FALLBACK_PATH "/spiffs/anim_manifest.bin"

#ifdef CONFIG_WATCHER_ANIM_SWITCH_FADE_MS
#define WATCHER_ANIM_SWITCH_FADE_MS CONFIG_WATCHER_ANIM_SWITCH_FADE_MS
#else
#define WATCHER_ANIM_SWITCH_FADE_MS 140
#endif

/* Emoji animation types */
typedef enum {
    EMOJI_ANIM_BOOT = 0, /* Boot animation (first play) */
    EMOJI_ANIM_HAPPY,
    EMOJI_ANIM_ERROR,
    EMOJI_ANIM_SPEAKING,
    EMOJI_ANIM_LISTENING,
    EMOJI_ANIM_PROCESSING,
    EMOJI_ANIM_STANDBY,
    EMOJI_ANIM_THINKING,
    EMOJI_ANIM_CUSTOM_1,
    EMOJI_ANIM_CUSTOM_2,
    EMOJI_ANIM_CUSTOM_3,
    EMOJI_ANIM_COUNT,
    EMOJI_ANIM_NONE = -1
} emoji_anim_type_t;

typedef struct {
    bool available;
    emoji_anim_type_t type;
    char name[24];
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    bool loop;
    uint16_t frame_count;
    char first_frame_raw[ANIM_MAX_PATH_LEN];
    char frame_paths[MAX_EMOJI_IMAGES][ANIM_MAX_PATH_LEN];
} anim_catalog_type_info_t;

/* ------------------------------------------------------------------ */
/* Legacy PNG API (retained for compatibility)                        */
/* ------------------------------------------------------------------ */

/* Image descriptor arrays for each emoji type */
extern lv_img_dsc_t *g_emoji_images[EMOJI_ANIM_COUNT][MAX_EMOJI_IMAGES];
extern int g_emoji_counts[EMOJI_ANIM_COUNT];

/**
 * @brief Initialize SPIFFS filesystem
 * @return 0 on success, -1 on error
 */
int emoji_spiffs_init(void);

/**
 * @brief Load all emoji images from SPIFFS (PNG format)
 *
 * Scans /spiffs directory for PNG files with specific prefixes:
 * - happy*.png
 * - error*.png
 * - speaking*.png
 * - listening*.png
 * - processing*.png
 * - standby*.png
 * - thinking*.png
 * - custom1*.png
 * - custom2*.png
 * - custom3*.png
 *
 * @return 0 on success, -1 on error
 */
int emoji_load_all_images(void);

/**
 * @brief Load PNG frames for a single emoji animation type
 *
 * If the type is already loaded, returns the current frame count.
 *
 * @param type Emoji animation type
 * @return Number of loaded frames, or -1 on error
 */
int emoji_load_type(emoji_anim_type_t type);

/**
 * @brief Get PNG image descriptor for specific emoji type and frame
 * @param type Emoji animation type
 * @param frame Frame index (0 to count-1)
 * @return Pointer to image descriptor, or NULL if invalid
 */
lv_img_dsc_t *emoji_get_image(emoji_anim_type_t type, int frame);

/**
 * @brief Get frame count for emoji type
 * @param type Emoji animation type
 * @return Number of frames available
 */
int emoji_get_frame_count(emoji_anim_type_t type);

/**
 * @brief Free all loaded PNG images
 */
void emoji_free_all(void);

/**
 * @brief Check if PNG images have been loaded
 * @return true if emoji_load_all_images* was already called successfully
 */
bool emoji_images_loaded(void);

/**
 * @brief Callback called after each emoji type finishes loading
 * @param type        The type just loaded
 * @param types_done  How many types have been loaded so far (1-based)
 * @param types_total Total number of types
 */
typedef void (*emoji_progress_cb_t)(emoji_anim_type_t type, int types_done, int types_total);

/**
 * @brief Load all emoji images from SPIFFS with per-type progress callback
 * @param cb  Progress callback (may be NULL)
 * @return 0 on success, -1 if no images loaded
 */
int emoji_load_all_images_with_cb(emoji_progress_cb_t cb);

/**
 * @brief Get emoji type name string
 * @param type Emoji type
 * @return Name string, or "unknown"
 */
const char *emoji_type_name(emoji_anim_type_t type);

/* ------------------------------------------------------------------ */
/* Animation catalog and cache API                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief Cached RGB565 frame data
 */
typedef struct {
    uint8_t *img_data;      /**< Decoded image data in LVGL native format (PSRAM) */
    size_t data_size;       /**< Size in bytes for the decoded image buffer */
    int width;              /**< Image width */
    int height;             /**< Image height */
    lv_img_cf_t color_format;
    lv_img_dsc_t img_dsc;   /**< Persistent LVGL descriptor for this frame */
} anim_cached_frame_t;

/**
 * @brief Cached animation type (all frames for one animation)
 */
typedef struct {
    anim_cached_frame_t *frames; /**< Array of cached frames */
    int frame_count;             /**< Number of cached frames */
    bool is_loaded;              /**< True if frames are valid */
    emoji_anim_type_t type;      /**< Animation type */
    uint32_t generation_id;      /**< Request generation used to build the cache */
    size_t total_bytes;          /**< Total bytes held by this cache */
} anim_type_cache_t;

typedef struct {
    bool loaded;
    emoji_anim_type_t type;
    size_t data_size;
    int width;
    int height;
    lv_img_cf_t color_format;
    uint8_t *img_data;
    lv_img_dsc_t img_dsc;
} anim_warm_frame_t;

/**
 * @brief Initialize SPIFFS, metadata, and the animation catalog.
 */
int anim_catalog_init(void);

/**
 * @brief Get manifest/catalog information for a type.
 */
const anim_catalog_type_info_t *anim_catalog_get_type_info(emoji_anim_type_t type);

/**
 * @brief Return true if a type is available in the catalog.
 */
bool anim_catalog_has_type(emoji_anim_type_t type);

/**
 * @brief Initialize the warm cache for all runtime animation types.
 */
int anim_warm_init_all_types(void);

/**
 * @brief Get a warm-cached first frame for the requested type.
 */
const lv_img_dsc_t *anim_warm_get_first_frame(emoji_anim_type_t type);

/**
 * @brief Prepare the inactive hot cache for a type.
 */
int anim_hot_build_type(emoji_anim_type_t type, uint32_t generation_id);

/**
 * @brief Commit the prepared hot cache and make it active.
 */
int anim_hot_commit_prepared(emoji_anim_type_t type, uint32_t generation_id);

/**
 * @brief Drop any prepared-but-not-committed hot cache.
 */
void anim_hot_discard_prepared(void);

/**
 * @brief Check if the active hot cache already matches the type.
 */
bool anim_hot_is_active_type(emoji_anim_type_t type);

/**
 * @brief Get the currently active hot cache type.
 */
emoji_anim_type_t anim_hot_get_active_type(void);

/**
 * @brief Get the active hot cache frame count.
 */
int anim_hot_get_frame_count(void);

/**
 * @brief Get a frame from the active hot cache.
 */
anim_cached_frame_t *anim_hot_get_frame(emoji_anim_type_t type, int frame);

/**
 * @brief Free all hot-cache memory.
 */
void anim_hot_free_all(void);

#endif /* ANIM_STORAGE_H */
