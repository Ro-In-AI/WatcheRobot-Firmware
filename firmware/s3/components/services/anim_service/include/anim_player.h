/**
 * @file emoji_anim.h
 * @brief Emoji animation timer system
 *
 * Provides frame-based animation for emoji images using LVGL timers.
 * Supports multiple animation states with configurable frame rates.
 */

#ifndef EMOJI_ANIM_H
#define EMOJI_ANIM_H

#include "emoji_png.h"
#include "lvgl.h"

/* Animation frame intervals in milliseconds */
#define EMOJI_ANIM_INTERVAL_MS   150   /* Optimized for smoother animation (was 200ms) */

/**
 * @brief Animation callback function type
 * @param img_dsc Image descriptor to display
 */
typedef void (*emoji_anim_callback_t)(lv_img_dsc_t *img_dsc);

/**
 * @brief Initialize animation system
 *
 * Must be called after emoji_load_all_images().
 *
 * @param img_obj LVGL image object to animate
 * @return 0 on success, -1 on error
 */
int emoji_anim_init(lv_obj_t *img_obj);

/**
 * @brief Start emoji animation
 *
 * Begins cycling through frames of the specified emoji type.
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

#endif /* EMOJI_ANIM_H */
