#ifndef HAL_DISPLAY_H
#define HAL_DISPLAY_H

#include <stdbool.h>

/**
 * Set text on display
 * @param text Text to display
 * @param font_size Font size
 * @return 0 on success, -1 on error
 */
int hal_display_set_text(const char *text, int font_size);

/**
 * Set text on display with an explicit text style.
 * @param text Text to display
 * @param font_size Font size
 * @param alert_text True for alert styling, false for normal styling
 * @return 0 on success, -1 on error
 */
int hal_display_set_text_with_style(const char *text, int font_size, bool alert_text);

/**
 * Set emoji image on display
 * @param emoji_id Emoji type ID (emoji_type_t)
 * @return 0 on success, -1 on error
 */
int hal_display_set_emoji(int emoji_id);

/**
 * Query the emoji that is actually being displayed right now.
 * Returns the display_ui emoji ID, or -1 if unavailable.
 */
int hal_display_get_current_emoji_id(void);

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

/**
 * @brief Initialize touch/knob inputs after the display is already up
 *
 * This is used to delay all interactive input devices until the app is ready.
 *
 * @return 0 on success, -1 on error
 */
int hal_display_input_init(void);

/**
 * @brief Return whether knob/encoder input is attached successfully
 */
bool hal_display_has_knob_input(void);

#endif /* HAL_DISPLAY_H */
