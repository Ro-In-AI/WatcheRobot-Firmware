/**
 * @file hal_display.c
 * @brief Display HAL implementation with SPIFFS-based emoji animation
 */

#include "hal_display.h"
#include "boot_animation.h"
#include "display_ui.h"
#include "emoji_png.h"
#include "emoji_anim.h"
#include "sensecap-watcher.h"
#include "esp_log.h"
#include "lvgl.h"

#if LV_USE_PNG
#include "lvgl/src/extra/libs/png/lv_png.h"
#endif

/* External CJK font for Chinese character support */
#if LV_FONT_SIMSUN_16_CJK
extern const lv_font_t lv_font_simsun_16_cjk;
#endif

#include "esp_lvgl_port.h"

#define TAG "HAL_DISPLAY"

static lv_obj_t *label_text = NULL;
static lv_obj_t *img_emoji = NULL;
static bool minimal_initialized = false;
static bool is_initialized = false;

/* Map display_ui emoji_type to emoji_png emoji_anim_type */
static emoji_anim_type_t map_emoji_type(int ui_emoji_id)
{
    /* display_ui.h: 0=standby, 1=happy, 2=sad, 3=surprised, 4=angry,
     *              5=listening, 6=analyzing, 7=speaking */
    /* emoji_png.h: 0=greeting, 1=detecting, 2=detected, 3=speaking,
     *              4=listening, 5=analyzing, 6=standby */
    /* Map display_ui emoji types to animation types */
    switch (ui_emoji_id) {
        case 0:  /* EMOJI_STANDBY */   return EMOJI_ANIM_STANDBY;    /* standby */
        case 1:  /* EMOJI_HAPPY */     return EMOJI_ANIM_GREETING;   /* greeting */
        case 2:  /* EMOJI_SAD */       return EMOJI_ANIM_DETECTED;   /* detected */
        case 3:  /* EMOJI_SURPRISED */ return EMOJI_ANIM_ANALYZING;  /* thinking */
        case 4:  /* EMOJI_ANGRY */     return EMOJI_ANIM_ANALYZING;  /* analyzing */
        case 5:  /* EMOJI_LISTENING */ return EMOJI_ANIM_LISTENING;  /* listening */
        case 6:  /* EMOJI_ANALYZING */ return EMOJI_ANIM_ANALYZING;  /* analyzing */
        case 7:  /* EMOJI_SPEAKING */  return EMOJI_ANIM_SPEAKING;   /* speaking */
        default: return EMOJI_ANIM_STANDBY;
    }
}

/* ------------------------------------------------------------------ */
/* Minimal init for boot animation                                            */
/* ------------------------------------------------------------------ */

int hal_display_minimal_init(void)
{
    if (minimal_initialized || is_initialized) {
        return 0;  /* Already done */
    }

    ESP_LOGI(TAG, "Minimal display init for boot animation...");

    /* 1. Initialize IO expander */
    if (bsp_io_expander_init() == NULL) {
        ESP_LOGE(TAG, "Failed to initialize IO expander");
        return -1;
    }
    ESP_LOGI(TAG, "IO expander initialized, LCD power ON");

    /* 2. Initialize LVGL via SDK */
    lv_disp_t *disp = bsp_lvgl_init();
    if (disp == NULL) {
        ESP_LOGE(TAG, "Failed to initialize LVGL");
        return -1;
    }
    ESP_LOGI(TAG, "LVGL initialized");

    /* 3. Set backlight brightness */
    esp_err_t ret = bsp_lcd_brightness_set(50);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set brightness: %d", ret);
    } else {
        ESP_LOGI(TAG, "Backlight set to 50%%");
    }

    /* 4. Initialize LVGL PNG decoder (needed for boot animation error emoji) */
#if LV_USE_PNG
    lv_png_init();
    ESP_LOGI(TAG, "PNG decoder initialized");
#endif

    /* Note: SPIFFS and emoji will be loaded later by hal_display_init() */

    minimal_initialized = true;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Full display initialization (with SPIFFS and emoji)            */
/* ------------------------------------------------------------------ */

int hal_display_init(void)
{
    if (is_initialized) {
        return 0;  /* Already initialized */
    }

    ESP_LOGI(TAG, "Full display initialization...");

    /* If minimal init was not done, do it now */
    if (bsp_io_expander_init() == NULL) {
        /* Already initialized check - try to get display */
        lv_disp_t *disp_check = lv_disp_get_default();
        if (disp_check == NULL) {
            /* Need to do minimal init first */
            if (hal_display_minimal_init() != 0) {
                return -1;
            }
        }
    }

    /* 1. Initialize SPIFFS and load emoji images */
    if (emoji_spiffs_init() != 0) {
        ESP_LOGW(TAG, "Failed to initialize SPIFFS, emoji animations disabled");
    } else {
        if (emoji_load_all_images() != 0) {
            ESP_LOGW(TAG, "Failed to load emoji images");
        } else {
            ESP_LOGI(TAG, "Emoji images loaded successfully");
        }
    }

    /* 2. Get current active screen */
    lv_obj_t *scr = lv_disp_get_scr_act(NULL);

    /* Set screen background to dark color */
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Disable scrolling/dragging on screen */
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    /* 6. Create emoji image FIRST (centered) - so it's in background */
    img_emoji = lv_img_create(scr);
    lv_obj_align(img_emoji, LV_ALIGN_CENTER, 0, 40);

    /* 7. Create text label AFTER emoji - so it's in foreground */
    label_text = lv_label_create(scr);
    lv_obj_set_width(label_text, 380);
    lv_label_set_long_mode(label_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label_text, LV_TEXT_ALIGN_CENTER, 0);  /* Center align text */
    lv_label_set_text(label_text, "Ready");
    lv_obj_set_style_text_color(label_text, lv_color_white(), 0);
    lv_obj_align(label_text, LV_ALIGN_CENTER, 0, -140);  /* Move higher to avoid emoji overlap */

    /* Set CJK font for Chinese character support */
#if LV_FONT_SIMSUN_16_CJK
    lv_obj_set_style_text_font(label_text, &lv_font_simsun_16_cjk, 0);
    ESP_LOGI(TAG, "Using SimSun 16 CJK font for Chinese support");
#else
    ESP_LOGW(TAG, "CJK font not enabled, Chinese characters may not display");
#endif

    /* 9. Initialize animation system */
    if (emoji_anim_init(img_emoji) == 0) {
        /* Start with greeting animation */
        emoji_anim_start(EMOJI_ANIM_GREETING);
    }

    is_initialized = true;
    ESP_LOGI(TAG, "Display initialized with LVGL and emoji animations");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Display UI init - called after boot animation finishes                */
/* ------------------------------------------------------------------ */

int hal_display_ui_init(void)
{
    if (is_initialized) {
        return 0;  /* Already initialized */
    }

    ESP_LOGI(TAG, "Initializing display UI...");

    /* 1. Minimal init is already done by hal_display_minimal_init() at boot.
     * hal_display_minimal_init() is idempotent and returns early if already done. */
    if (hal_display_minimal_init() != 0) {
        ESP_LOGE(TAG, "Failed minimal display init");
        return -1;
    }

    /* 2. Load emoji images if not already loaded by app_main boot sequence */
    if (!emoji_images_loaded()) {
        ESP_LOGI(TAG, "Loading emoji images (fallback)...");
        if (emoji_spiffs_init() != 0) {
            ESP_LOGW(TAG, "Failed to initialize SPIFFS, emoji animations disabled");
        } else if (emoji_load_all_images() != 0) {
            ESP_LOGW(TAG, "Failed to load emoji images");
        } else {
            ESP_LOGI(TAG, "Emoji images loaded successfully");
        }
    } else {
        ESP_LOGI(TAG, "Emoji images already loaded");
    }

    /* 3. Get old boot screen before locking (for deferred deletion) */
    lv_obj_t *old_boot_scr = boot_anim_get_screen();

    /* 4. Create new main screen and load it (under LVGL lock) */
    lvgl_port_lock(0);

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    /* 4. Create emoji image FIRST (centered) - so it's in background */
    img_emoji = lv_img_create(scr);
    lv_obj_align(img_emoji, LV_ALIGN_CENTER, 0, 40);

    /* 5. Create text label AFTER emoji - so it's in foreground */
    label_text = lv_label_create(scr);
    lv_obj_set_width(label_text, 380);
    lv_label_set_long_mode(label_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label_text, "Ready");
    lv_obj_set_style_text_color(label_text, lv_color_white(), 0);
    lv_obj_align(label_text, LV_ALIGN_CENTER, 0, -140);

    /* Set CJK font for Chinese character support */
#if LV_FONT_SIMSUN_16_CJK
    lv_obj_set_style_text_font(label_text, &lv_font_simsun_16_cjk, 0);
    ESP_LOGI(TAG, "Using SimSun 16 CJK font for Chinese support");
#else
    ESP_LOGW(TAG, "CJK font not enabled, Chinese characters may not display");
#endif

    /* 6. Initialize animation system */
    if (emoji_anim_init(img_emoji) == 0) {
        emoji_anim_start(EMOJI_ANIM_GREETING);
    }

    /* Load the new main screen - this makes scr the active screen */
    lv_disp_load_scr(scr);

    /* Now safe to delete the old boot screen (it's no longer active) */
    if (old_boot_scr) {
        lv_obj_del(old_boot_scr);
    }

    lvgl_port_unlock();

    is_initialized = true;
    ESP_LOGI(TAG, "Display UI initialized with LVGL and emoji animations");
    return 0;
}

int hal_display_set_text(const char *text, int font_size)
{
    if (!is_initialized || !label_text) {
        ESP_LOGW(TAG, "Display not initialized");
        return -1;
    }

    if (!text) {
        return -1;
    }

    #define MAX_DISPLAY_CHARS 30
    char truncated[MAX_DISPLAY_CHARS + 4];
    int len = strlen(text);

    if (len > MAX_DISPLAY_CHARS) {
        strncpy(truncated, text, MAX_DISPLAY_CHARS);
        strcpy(truncated + MAX_DISPLAY_CHARS, "...");
        ESP_LOGI(TAG, "Set text (truncated): '%s' -> '%s'", text, truncated);
        lvgl_port_lock(0);
        lv_label_set_text(label_text, truncated);
        lvgl_port_unlock();
    } else {
        ESP_LOGI(TAG, "Set text: '%s' (size %d)", text, font_size);
        lvgl_port_lock(0);
        lv_label_set_text(label_text, text);
        lvgl_port_unlock();
    }

    return 0;
}

int hal_display_set_emoji(int emoji_id)
{
    if (!is_initialized || !img_emoji) {
        ESP_LOGW(TAG, "Display not initialized");
        return -1;
    }

    /* Map UI emoji type to animation type */
    emoji_anim_type_t type = map_emoji_type(emoji_id);

    /* emoji_anim_start calls LVGL APIs - must hold lock */
    lvgl_port_lock(0);
    int ret = emoji_anim_start(type);
    lvgl_port_unlock();
    if (ret != 0) {
        ESP_LOGW(TAG, "Failed to start animation for emoji ID: %d", emoji_id);
        return -1;
    }

    const char *emoji_name = "unknown";
    switch (emoji_id) {
        case 0:  emoji_name = "standby"; break;
        case 1:  emoji_name = "happy"; break;
        case 2:  emoji_name = "sad"; break;
        case 3:  emoji_name = "surprised"; break;
        case 4:  emoji_name = "angry"; break;
        case 5:  emoji_name = "listening"; break;
        case 6:  emoji_name = "analyzing"; break;
        case 7:  emoji_name = "speaking"; break;
    }

    ESP_LOGI(TAG, "Set emoji: %s -> %s animation", emoji_name, emoji_type_name(type));
    return 0;
}

/**
 * @brief Start speaking animation (for voice interaction)
 */
int hal_display_start_speaking(void)
{
    if (!is_initialized) return -1;
    lvgl_port_lock(0);
    int ret = emoji_anim_start(EMOJI_ANIM_SPEAKING);
    lvgl_port_unlock();
    return ret;
}

int hal_display_start_listening(void)
{
    if (!is_initialized) return -1;
    lvgl_port_lock(0);
    int ret = emoji_anim_start(EMOJI_ANIM_LISTENING);
    lvgl_port_unlock();
    return ret;
}

int hal_display_start_analyzing(void)
{
    if (!is_initialized) return -1;
    lvgl_port_lock(0);
    int ret = emoji_anim_start(EMOJI_ANIM_ANALYZING);
    lvgl_port_unlock();
    return ret;
}

int hal_display_stop_animation(void)
{
    if (!is_initialized) return -1;
    lvgl_port_lock(0);
    int ret = emoji_anim_start(EMOJI_ANIM_STANDBY);
    lvgl_port_unlock();
    return ret;
}
