/**
 * @file boot_animation.h
 * @brief Boot animation with arc progress bar and error handling
 *
 * Displays a circular progress bar during system initialization.
 * On error, shows sad emoji and countdown to reboot.
 */

#ifndef BOOT_ANIMATION_H
#define BOOT_ANIMATION_H

#include <stdint.h>
#include "lvgl.h"

/**
 * @brief Initialize and show boot animation screen
 *
 * Creates a full-screen black background with:
 * - Circular arc progress bar (412x412)
 * - Percentage text (0% - 100%)
 * - Status text label
 */
void boot_anim_init(void);

/**
 * @brief Set progress bar value
 *
 * @param percent Progress value (0-100)
 */
void boot_anim_set_progress(int percent);

/**
 * @brief Set status text above progress bar
 *
 * @param text Status message (e.g., "WiFi...", "Cloud...")
 */
void boot_anim_set_text(const char *text);

/**
 * @brief Show error screen and countdown to reboot
 *
 * Displays:
 * - Sad emoji image
 * - Error message text
 * - Countdown timer (10s...1s)
 * - Reboots device after countdown
 *
 * @param error_msg Error message to display
 */
void boot_anim_show_error(const char *error_msg);

/**
 * @brief Finish boot animation and prepare for main UI
 *
 * Clears child widget pointers but does NOT delete the screen.
 * The caller must call boot_anim_get_screen() to get the old screen
 * and delete it AFTER loading a new screen via lv_disp_load_scr().
 */
void boot_anim_finish(void);

/**
 * @brief Get the boot screen object (for deferred cleanup)
 *
 * Returns the boot screen so the caller can delete it after
 * loading a new screen. Returns NULL after cleanup.
 *
 * @return Pointer to boot screen, or NULL if already cleaned up
 */
lv_obj_t *boot_anim_get_screen(void);

#endif /* BOOT_ANIMATION_H */
