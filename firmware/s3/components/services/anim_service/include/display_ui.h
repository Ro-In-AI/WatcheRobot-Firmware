#ifndef DISPLAY_UI_H
#define DISPLAY_UI_H

#include <stdbool.h>
#include <stdint.h>

/* Supported UI emoji types - mapped to unified animation names */
typedef enum {
    EMOJI_STANDBY = 0, /* standby - idle/default state */
    EMOJI_HAPPY,       /* happy */
    EMOJI_LISTENING,   /* listening - recording user voice */
    EMOJI_THINKING,    /* thinking - short transitional state */
    EMOJI_PROCESSING,  /* processing - AI/task execution */
    EMOJI_SPEAKING,    /* speaking - TTS playback */
    EMOJI_ERROR,       /* error - failure/interruption */
    EMOJI_CUSTOM_1,    /* reserved custom state */
    EMOJI_CUSTOM_2,    /* reserved custom state */
    EMOJI_CUSTOM_3,    /* reserved custom state */
    EMOJI_COUNT,       /* Sentinel */
    EMOJI_UNKNOWN = -1,
} emoji_type_t;

/* Display update result */
typedef struct {
    int text_updated;  /* Non-zero if text was updated */
    int emoji_updated; /* Non-zero if emoji was updated */
    int emoji_id;      /* Emoji ID that was set */
} display_result_t;

/**
 * Initialize display UI
 */
void display_ui_init(void);

/**
 * Map emoji string to enum
 * @param emoji_str String like "happy", "processing", "error", etc.
 * @return Emoji type enum, or EMOJI_UNKNOWN if invalid
 */
emoji_type_t display_emoji_from_string(const char *emoji_str);

/**
 * Update display with text and optional emoji
 * @param text Text to display (can be NULL)
 * @param emoji Emoji string like "happy", "processing", "error" (can be NULL)
 * @param font_size Font size (0 for default)
 * @param out_result Output result (can be NULL)
 * @return 0 on success, -1 on error
 */
int display_update(const char *text, const char *emoji, int font_size, display_result_t *out_result);

/**
 * Get current text
 * @param out_buf Output buffer
 * @param buf_size Buffer size
 * @return 0 on success, -1 if no text or error
 */
int display_get_text(char *out_buf, int buf_size);

/**
 * Get current emoji type
 */
emoji_type_t display_get_emoji(void);

/* ------------------------------------------------------------------ */
/* HAL Interface (to be implemented by LVGL layer)                    */
/* ------------------------------------------------------------------ */

/**
 * Set text on display (HAL)
 * @param text Text to display
 * @param font_size Font size
 * @return 0 on success, -1 on error
 */
int hal_display_set_text(const char *text, int font_size);

/**
 * Set emoji image on display (HAL)
 * @param emoji_id Emoji type ID (emoji_type_t)
 * @return 0 on success, -1 on error
 */
int hal_display_set_emoji(int emoji_id);

#endif /* DISPLAY_UI_H */
