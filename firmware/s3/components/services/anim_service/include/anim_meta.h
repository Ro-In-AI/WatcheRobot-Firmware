/**
 * @file anim_meta.h
 * @brief Animation metadata configuration system
 *
 * Provides runtime configuration for animation parameters (FPS, loop, frame count)
 * loaded from anim_meta.json in SPIFFS. Falls back to defaults if file missing.
 */

#ifndef ANIM_META_H
#define ANIM_META_H

#include "anim_storage.h"
#include <stdbool.h>

/**
 * @brief Per-animation-type configuration
 */
typedef struct {
    int frame_count;    /**< Number of frames (0 = auto-detect from files) */
    bool loop;          /**< Whether animation should loop */
    int fps;            /**< Frames per second for this type (0 = use default) */
} anim_config_t;

/**
 * @brief Global animation metadata
 */
typedef struct {
    char version[16];       /**< Metadata format version */
    int default_fps;        /**< Default FPS for all animations */
    anim_config_t animations[EMOJI_ANIM_COUNT]; /**< Per-type config */
} anim_meta_t;

/**
 * @brief Initialize animation metadata system
 *
 * Loads anim_meta.json from /spiffs/anim/ directory.
 * Falls back to compiled defaults if file not found.
 *
 * @return 0 on success, -1 on error
 */
int anim_meta_init(void);

/**
 * @brief Get configuration for specific animation type
 *
 * @param type Animation type
 * @return Pointer to config (never NULL, returns defaults if not initialized)
 */
anim_config_t* anim_meta_get_config(emoji_anim_type_t type);

/**
 * @brief Get effective FPS for animation type
 *
 * Returns type-specific FPS if set, otherwise default FPS.
 *
 * @param type Animation type
 * @return FPS value (default 30 if not configured)
 */
int anim_meta_get_fps(emoji_anim_type_t type);

/**
 * @brief Check if animation type should loop
 *
 * @param type Animation type
 * @return true if should loop, false otherwise
 */
bool anim_meta_should_loop(emoji_anim_type_t type);

/**
 * @brief Get default FPS value
 *
 * @return Default FPS (30 if not configured)
 */
int anim_meta_get_default_fps(void);

#endif /* ANIM_META_H */
