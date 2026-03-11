#ifndef HAL_DISPLAY_H
#define HAL_DISPLAY_H

/**
 * Set text on display
 * @param text Text to display
 * @param font_size Font size
 * @return 0 on success, -1 on error
 */
int hal_display_set_text(const char *text, int font_size);

/**
 * Set emoji image on display
 * @param emoji_id Emoji type ID (emoji_type_t)
 * @return 0 on success, -1 on error
 */
int hal_display_set_emoji(int emoji_id);

/**
 * @brief Minimal display init for boot animation
 *
 * Initializes only IO expander and LVGL, and backlight.
 * Does NOT load SPIFFS or emoji images.
 *
 * @return 0 on success, -1 on error
 */
int hal_display_minimal_init(void);

/**
 * @brief Full display initialization
 *
 * Initializes everything including SPIFFS and emoji images.
 *
 * @return 0 on success, -1 on error
 */
int hal_display_init(void);

/**
 * @brief Display UI init - called after boot animation finishes
 *
 * Creates main UI with emoji animation and text label.
 * Call hal_display_minimal_init() internally if needed.
 *
 * @return 0 on success, -1 on error
 */
int hal_display_ui_init(void);

#endif /* HAL_DISPLAY_H */
