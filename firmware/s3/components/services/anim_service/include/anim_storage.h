/**
 * @file anim_storage.h
 * @brief Emoji PNG image loader and RGB565 cache system
 *
 * Loads PNG images from SPIFFS storage and provides:
 * 1. Raw PNG data via lv_img_dsc_t (for LVGL PNG decoder)
 * 2. Decoded RGB565 cache (for 30fps fast frame switching)
 *
 * Supports multiple emoji animation types with frame sequences.
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
#define MAX_EMOJI_IMAGES    18
#endif

/* Emoji animation types */
typedef enum {
    EMOJI_ANIM_BOOT = 0,       /* Boot animation (first play) */
    EMOJI_ANIM_GREETING,
    EMOJI_ANIM_DETECTING,
    EMOJI_ANIM_DETECTED,
    EMOJI_ANIM_SPEAKING,
    EMOJI_ANIM_LISTENING,
    EMOJI_ANIM_ANALYZING,
    EMOJI_ANIM_STANDBY,
    EMOJI_ANIM_COUNT,
    EMOJI_ANIM_NONE = -1
} emoji_anim_type_t;

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
 * - greeting*.png
 * - detecting*.png
 * - detected*.png
 * - speaking*.png
 * - listening*.png
 * - analyzing*.png
 * - standby*.png
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
lv_img_dsc_t* emoji_get_image(emoji_anim_type_t type, int frame);

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
const char* emoji_type_name(emoji_anim_type_t type);

/* ------------------------------------------------------------------ */
/* RGB565 Cache API (for 30fps playback)                              */
/* ------------------------------------------------------------------ */

/**
 * @brief Cached RGB565 frame data
 */
typedef struct {
    uint8_t *img_data;          /**< Decoded image data in LVGL native format (PSRAM) */
    size_t data_size;           /**< Size in bytes for the decoded image buffer */
    int width;                  /**< Image width */
    int height;                 /**< Image height */
    lv_img_dsc_t img_dsc;       /**< Persistent LVGL descriptor for this frame */
} anim_cached_frame_t;

/**
 * @brief Cached animation type (all frames for one animation)
 */
typedef struct {
    anim_cached_frame_t *frames;    /**< Array of cached frames */
    int frame_count;                /**< Number of cached frames */
    bool is_loaded;                 /**< True if frames are valid */
    emoji_anim_type_t type;         /**< Animation type */
} anim_type_cache_t;

/**
 * @brief Initialize RGB565 cache system
 *
 * Must be called before anim_cache_load_type().
 *
 * @return 0 on success, -1 on error
 */
int anim_cache_init(void);

/**
 * @brief Load and cache all frames for an animation type
 *
 * Decodes PNG files to RGB565 format and stores in PSRAM.
 * If another type is cached, it will be unloaded first.
 *
 * @param type Animation type to load
 * @return 0 on success, -1 on error
 */
int anim_cache_load_type(emoji_anim_type_t type);

/**
 * @brief Unload cached frames for an animation type
 *
 * Frees PSRAM memory used by cached frames.
 *
 * @param type Animation type to unload
 */
void anim_cache_unload_type(emoji_anim_type_t type);

/**
 * @brief Get cached RGB565 frame
 *
 * @param type Animation type
 * @param frame Frame index
 * @return Pointer to cached frame, or NULL if not cached/invalid
 */
anim_cached_frame_t* anim_cache_get_frame(emoji_anim_type_t type, int frame);

/**
 * @brief Check if animation type is cached
 *
 * @param type Animation type
 * @return true if type is loaded in cache
 */
bool anim_cache_is_type_loaded(emoji_anim_type_t type);

/**
 * @brief Get currently cached type
 *
 * @return Currently cached type, or EMOJI_ANIM_NONE if none
 */
emoji_anim_type_t anim_cache_get_current_type(void);

/**
 * @brief Get cached frame count for current type
 *
 * @return Frame count, or 0 if no type cached
 */
int anim_cache_get_frame_count(void);

/**
 * @brief Free all cached frames
 */
void anim_cache_free_all(void);

#endif /* ANIM_STORAGE_H */
