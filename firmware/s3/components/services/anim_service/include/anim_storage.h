/**
 * @file emoji_png.h
 * @brief Emoji PNG image loader from SPIFFS
 *
 * Loads PNG images from SPIFFS storage and converts to LVGL image descriptors.
 * Supports multiple emoji animation types with frame sequences.
 */

#ifndef EMOJI_PNG_H
#define EMOJI_PNG_H

#include "lvgl.h"
#include <stdbool.h>

#define MAX_EMOJI_IMAGES    10

/* Emoji animation types */
typedef enum {
    EMOJI_ANIM_GREETING = 0,
    EMOJI_ANIM_DETECTING,
    EMOJI_ANIM_DETECTED,
    EMOJI_ANIM_SPEAKING,
    EMOJI_ANIM_LISTENING,
    EMOJI_ANIM_ANALYZING,
    EMOJI_ANIM_STANDBY,
    EMOJI_ANIM_COUNT,
    EMOJI_ANIM_NONE = -1
} emoji_anim_type_t;

/* Image descriptor arrays for each emoji type */
extern lv_img_dsc_t *g_emoji_images[EMOJI_ANIM_COUNT][MAX_EMOJI_IMAGES];
extern int g_emoji_counts[EMOJI_ANIM_COUNT];

/**
 * @brief Initialize SPIFFS filesystem
 * @return 0 on success, -1 on error
 */
int emoji_spiffs_init(void);

/**
 * @brief Load all emoji images from SPIFFS
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
 * @brief Get image descriptor for specific emoji type and frame
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
 * @brief Free all loaded emoji images
 */
void emoji_free_all(void);

/**
 * @brief Check if emoji images have been loaded
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

#endif /* EMOJI_PNG_H */
