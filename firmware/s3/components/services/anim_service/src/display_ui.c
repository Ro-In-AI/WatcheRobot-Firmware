#include "display_ui.h"
#include "hal_display.h"
#include <string.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/* Private: Current state                                             */
/* ------------------------------------------------------------------ */

#define MAX_TEXT_LEN  128

static char g_current_text[MAX_TEXT_LEN] = {0};
static emoji_type_t g_current_emoji = EMOJI_STANDBY;
static const int DEFAULT_FONT_SIZE = 24;

/* ------------------------------------------------------------------ */
/* Private: Case-insensitive string compare                           */
/* ------------------------------------------------------------------ */

static int strcasecmp_local(const char *a, const char *b)
{
    if (!a || !b) return (a != b) ? 1 : 0;

    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

/* ------------------------------------------------------------------ */
/* Public: Initialize                                                 */
/* ------------------------------------------------------------------ */

void display_ui_init(void)
{
    memset(g_current_text, 0, sizeof(g_current_text));
    g_current_emoji = EMOJI_STANDBY;

    /* Initialize HAL display */
    hal_display_init();
}

/* ------------------------------------------------------------------ */
/* Public: Map emoji string to enum                                   */
/* ------------------------------------------------------------------ */

emoji_type_t display_emoji_from_string(const char *emoji_str)
{
    if (!emoji_str) {
        return EMOJI_UNKNOWN;
    }

    /* Success/Happy states */
    if (strcasecmp_local(emoji_str, "happy") == 0 ||
        strcasecmp_local(emoji_str, "success") == 0) {
        return EMOJI_HAPPY;
    }

    /* Error/Sad states */
    if (strcasecmp_local(emoji_str, "sad") == 0 ||
        strcasecmp_local(emoji_str, "error") == 0) {
        return EMOJI_SAD;
    }

    /* Thinking/Processing states */
    if (strcasecmp_local(emoji_str, "thinking") == 0 ||
        strcasecmp_local(emoji_str, "analyzing") == 0) {
        return EMOJI_ANALYZING;
    }

    /* Listening/Recording states */
    if (strcasecmp_local(emoji_str, "listening") == 0) {
        return EMOJI_LISTENING;
    }

    /* Speaking/TTS states */
    if (strcasecmp_local(emoji_str, "speaking") == 0) {
        return EMOJI_SPEAKING;
    }

    /* Standby/Idle states */
    if (strcasecmp_local(emoji_str, "standby") == 0 ||
        strcasecmp_local(emoji_str, "idle") == 0 ||
        strcasecmp_local(emoji_str, "normal") == 0) {
        return EMOJI_STANDBY;
    }

    /* Legacy mappings (kept for compatibility) */
    if (strcasecmp_local(emoji_str, "surprised") == 0) {
        return EMOJI_SURPRISED;
    }
    if (strcasecmp_local(emoji_str, "angry") == 0) {
        return EMOJI_ANGRY;
    }

    return EMOJI_UNKNOWN;
}

/* ------------------------------------------------------------------ */
/* Public: Update display                                             */
/* ------------------------------------------------------------------ */

int display_update(const char *text, const char *emoji, int font_size,
                   display_result_t *out_result)
{
    if (out_result) {
        memset(out_result, 0, sizeof(*out_result));
    }

    /* Update text if provided */
    if (text) {
        int fs = (font_size > 0) ? font_size : DEFAULT_FONT_SIZE;
        if (hal_display_set_text(text, fs) != 0) {
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
    if (emoji) {
        emoji_type_t emoji_id = display_emoji_from_string(emoji);
        if (emoji_id == EMOJI_UNKNOWN) {
            emoji_id = EMOJI_STANDBY;  /* Fallback to standby */
        }

        if (hal_display_set_emoji((int)emoji_id) != 0) {
            return -1;
        }
        g_current_emoji = emoji_id;

        if (out_result) {
            out_result->emoji_updated = 1;
            out_result->emoji_id = (int)emoji_id;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Public: Get current text                                           */
/* ------------------------------------------------------------------ */

int display_get_text(char *out_buf, int buf_size)
{
    if (!out_buf || buf_size <= 0) {
        return -1;
    }

    if (g_current_text[0] == '\0') {
        return -1;  /* No text set */
    }

    strncpy(out_buf, g_current_text, buf_size - 1);
    out_buf[buf_size - 1] = '\0';

    return 0;
}

/* ------------------------------------------------------------------ */
/* Public: Get current emoji                                          */
/* ------------------------------------------------------------------ */

emoji_type_t display_get_emoji(void)
{
    return g_current_emoji;
}
