/**
 * @file anim_player.h
 * @brief Animation player with 30fps playback support
 *
 * Provides frame-based animation for emoji images using LVGL timers.
 * Supports multiple animation states with configurable frame rates.
 *
 * Features:
 * - 30fps default frame rate (configurable via Kconfig)
 * - RGB565 cache for fast frame switching (<1ms latency)
 * - Lazy loading: only current animation type in PSRAM
 * - Type switch latency: <500ms
 */

#ifndef ANIM_PLAYER_H
#define ANIM_PLAYER_H

#include "anim_storage.h"
#include "lvgl.h"

/* Default animation frame interval in milliseconds (configurable via Kconfig) */
#ifdef CONFIG_WATCHER_ANIM_FPS
#define EMOJI_ANIM_INTERVAL_MS   (1000 / CONFIG_WATCHER_ANIM_FPS)
#else
#define EMOJI_ANIM_INTERVAL_MS   33  /* 30fps */
#endif

/**
 * @brief Animation callback function type
 * @param img_dsc Image descriptor to display
 */
typedef void (*emoji_anim_callback_t)(lv_img_dsc_t *img_dsc);

/**
 * @brief Initialize animation system
 *
 * Must be called after SPIFFS is initialized.
 * Initializes metadata system and RGB565 cache.
 *
 * @param img_obj LVGL image object to animate
 * @return 0 on success, -1 on error
 */
int emoji_anim_init(lv_obj_t *img_obj);

/**
 * @brief Start emoji animation
 *
 * Begins cycling through frames of the specified emoji type.
 * Automatically loads and caches RGB565 frames if not already cached.
 *
 * @param type Emoji animation type
 * @return 0 on success, -1 on error
 */
int emoji_anim_start(emoji_anim_type_t type);

/**
 * @brief Stop current animation
 */
void emoji_anim_stop(void);

/**
 * @brief Check if animation is running
 * @return true if animation is active, false otherwise
 */
bool emoji_anim_is_running(void);

/**
 * @brief Get current animation type
 * @return Current emoji type, or EMOJI_ANIM_NONE if stopped
 */
emoji_anim_type_t emoji_anim_get_type(void);

/**
 * @brief Set animation frame interval
 * @param interval_ms Interval between frames in milliseconds
 */
void emoji_anim_set_interval(uint32_t interval_ms);

/**
 * @brief Display single static emoji (no animation)
 * @param type Emoji type
 * @param frame Frame index (0 for first frame)
 * @return 0 on success, -1 on error
 */
int emoji_anim_show_static(emoji_anim_type_t type, int frame);

/**
 * @brief Prefetch animation type into cache
 *
 * Loads RGB565 frames for the specified type into PSRAM.
 * Use this to reduce latency when switching animations.
 *
 * @param type Animation type to prefetch
 * @return 0 on success, -1 on error
 */
int emoji_anim_prefetch_type(emoji_anim_type_t type);

/**
 * @brief Get current FPS setting
 * @return Frames per second
 */
int emoji_anim_get_fps(void);

/**
 * @brief Set FPS for current animation
 * @param fps Frames per second (1-60)
 */
void emoji_anim_set_fps(int fps);

#endif /* ANIM_PLAYER_H */
