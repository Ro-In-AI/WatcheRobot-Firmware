#include "display_ui.h"
#include "hal_display.h"
#include "esp_log.h"
#include <ctype.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Private: Current state                                             */
/* ------------------------------------------------------------------ */

#define MAX_TEXT_LEN 128
#define TAG "DISPLAY_UI"

static char g_current_text[MAX_TEXT_LEN] = {0};
static emoji_type_t g_current_emoji = EMOJI_STANDBY;
static const int DEFAULT_FONT_SIZE = 24;

static emoji_type_t sync_current_emoji_from_hal(void) {
    int actual_emoji = hal_display_get_current_emoji_id();
    if (actual_emoji >= 0 && actual_emoji < EMOJI_COUNT) {
        g_current_emoji = (emoji_type_t)actual_emoji;
    }
    return g_current_emoji;
}

static int text_equals_current(const char *text) {
    if (text == NULL) {
        return 0;
    }

    return strncmp(g_current_text, text, MAX_TEXT_LEN) == 0;
}

/* ------------------------------------------------------------------ */
/* Private: Case-insensitive string compare                           */
/* ------------------------------------------------------------------ */

static int strcasecmp_local(const char *a, const char *b) {
    if (!a || !b)
        return (a != b) ? 1 : 0;

    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb)
            return ca - cb;
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

/* ------------------------------------------------------------------ */
/* Public: Initialize                                                 */
/* ------------------------------------------------------------------ */

void display_ui_init(void) {
    memset(g_current_text, 0, sizeof(g_current_text));
    g_current_emoji = EMOJI_STANDBY;

    /* Initialize HAL display */
    hal_display_init();
    sync_current_emoji_from_hal();
}

/* ------------------------------------------------------------------ */
/* Public: Map emoji string to enum                                   */
/* ------------------------------------------------------------------ */

emoji_type_t display_emoji_from_string(const char *emoji_str) {
    if (!emoji_str) {
        return EMOJI_UNKNOWN;
    }

    if (strcasecmp_local(emoji_str, "standby") == 0 || strcasecmp_local(emoji_str, "idle") == 0 ||
        strcasecmp_local(emoji_str, "normal") == 0) {
        return EMOJI_STANDBY;
    }

    if (strcasecmp_local(emoji_str, "happy") == 0 || strcasecmp_local(emoji_str, "success") == 0) {
        return EMOJI_HAPPY;
    }

    if (strcasecmp_local(emoji_str, "listening") == 0) {
        return EMOJI_LISTENING;
    }

    if (strcasecmp_local(emoji_str, "thinking") == 0) {
        return EMOJI_THINKING;
    }

    if (strcasecmp_local(emoji_str, "processing") == 0 || strcasecmp_local(emoji_str, "analyzing") == 0) {
        return EMOJI_PROCESSING;
    }

    if (strcasecmp_local(emoji_str, "speaking") == 0) {
        return EMOJI_SPEAKING;
    }

    if (strcasecmp_local(emoji_str, "error") == 0) {
        return EMOJI_ERROR;
    }

    if (strcasecmp_local(emoji_str, "custom1") == 0) {
        return EMOJI_CUSTOM_1;
    }
    if (strcasecmp_local(emoji_str, "custom2") == 0) {
        return EMOJI_CUSTOM_2;
    }
    if (strcasecmp_local(emoji_str, "custom3") == 0) {
        return EMOJI_CUSTOM_3;
    }

    return EMOJI_UNKNOWN;
}

/* ------------------------------------------------------------------ */
/* Public: Update display                                             */
/* ------------------------------------------------------------------ */

int display_update_with_style(const char *text,
                              const char *emoji,
                              int font_size,
                              display_text_style_t text_style,
                              display_result_t *out_result) {
    emoji_type_t requested_emoji = EMOJI_UNKNOWN;
    int text_changed = 0;
    int emoji_changed = 0;
    int emoji_request_attempted = 0;
    emoji_type_t previous_emoji = sync_current_emoji_from_hal();
    emoji_type_t actual_emoji = previous_emoji;

    if (out_result) {
        memset(out_result, 0, sizeof(*out_result));
    }

    if (emoji) {
        requested_emoji = display_emoji_from_string(emoji);
        if (requested_emoji == EMOJI_UNKNOWN) {
            ESP_LOGW(TAG, "Unknown emoji '%s', falling back to standby", emoji);
            requested_emoji = EMOJI_STANDBY;
        }
    }

    text_changed = (text != NULL && !text_equals_current(text));
    emoji_changed = (emoji != NULL && requested_emoji != previous_emoji);

    /* Update text if provided */
    if (text_changed) {
        int fs = (font_size > 0) ? font_size : DEFAULT_FONT_SIZE;
        if (hal_display_set_text_with_style(text, fs, text_style == DISPLAY_TEXT_STYLE_ALERT) != 0) {
            return -1;
        }
        /* Store current text */
        strncpy(g_current_text, text, MAX_TEXT_LEN - 1);
        g_current_text[MAX_TEXT_LEN - 1] = '\0';

        if (out_result) {
            out_result->text_updated = 1;
        }
    }

    /* Update emoji if provided */
    if (emoji_changed) {
        emoji_request_attempted = 1;
        if (hal_display_set_emoji((int)requested_emoji) != 0) {
            return -1;
        }
        actual_emoji = sync_current_emoji_from_hal();

        if (out_result && actual_emoji != previous_emoji) {
            out_result->emoji_updated = 1;
            out_result->emoji_id = (int)actual_emoji;
        }
    }

    if (text != NULL || emoji != NULL) {
        if (!text_changed && !emoji_changed) {
            ESP_LOGI(TAG, "Display update no-op text=%s emoji=%s current_emoji=%d",
                     text != NULL ? "unchanged" : "skipped",
                     emoji != NULL ? "unchanged" : "skipped",
                     (int)g_current_emoji);
        } else if (emoji_request_attempted && actual_emoji != requested_emoji) {
            if (text_changed) {
                ESP_LOGI(TAG,
                         "Display update applied text; emoji pending requested=%d current_emoji=%d",
                         (int)requested_emoji,
                         (int)actual_emoji);
            } else {
                ESP_LOGI(TAG,
                         "Display update accepted emoji request pending requested=%d current_emoji=%d",
                         (int)requested_emoji,
                         (int)actual_emoji);
            }
        } else if (text_changed && (actual_emoji != previous_emoji)) {
            ESP_LOGI(TAG, "Display update applied text+emoji emoji_id=%d", (int)actual_emoji);
        } else if (text_changed) {
            ESP_LOGI(TAG, "Display update applied text only");
        } else {
            ESP_LOGI(TAG, "Display update applied emoji only emoji_id=%d", (int)actual_emoji);
        }
    }

    return 0;
}

int display_update(const char *text, const char *emoji, int font_size, display_result_t *out_result) {
    return display_update_with_style(text, emoji, font_size, DISPLAY_TEXT_STYLE_NORMAL, out_result);
}

/* ------------------------------------------------------------------ */
/* Public: Get current text                                           */
/* ------------------------------------------------------------------ */

int display_get_text(char *out_buf, int buf_size) {
    if (!out_buf || buf_size <= 0) {
        return -1;
    }

    if (g_current_text[0] == '\0') {
        return -1; /* No text set */
    }

    strncpy(out_buf, g_current_text, buf_size - 1);
    out_buf[buf_size - 1] = '\0';

    return 0;
}

/* ------------------------------------------------------------------ */
/* Public: Get current emoji                                          */
/* ------------------------------------------------------------------ */

emoji_type_t display_get_emoji(void) {
    return sync_current_emoji_from_hal();
}
