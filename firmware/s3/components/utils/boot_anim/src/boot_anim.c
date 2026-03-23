/**
 * @file boot_animation.c
 * @brief Boot animation with arc progress bar and error handling
 */

#include "boot_anim.h"
#include "anim_storage.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#define TAG "BOOT_ANIM"

/* Private objects */
static lv_obj_t *boot_screen = NULL;
static lv_obj_t *progress_arc = NULL;
static lv_obj_t *percent_label = NULL;
static lv_obj_t *status_label = NULL;
static lv_obj_t *error_img = NULL;
static lv_obj_t *countdown_label = NULL;
static lv_obj_t *intro_img = NULL;
static lv_timer_t *intro_timer = NULL;

/* Countdown state */
static int countdown_seconds = 10;
static bool in_error_mode = false;
static emoji_anim_type_t intro_type = EMOJI_ANIM_NONE;
static int intro_frame = 0;
static int intro_frame_count = 0;

/* Forward declarations */
static void boot_anim_countdown_task(void *param);
static void boot_anim_intro_timer_cb(lv_timer_t *timer);
static void boot_anim_stop_intro(void);

/* ------------------------------------------------------------------ */
/* Public: Initialize boot animation                                   */
/* ------------------------------------------------------------------ */

void boot_anim_init(void) {
    lvgl_port_lock(0);

    /* Create full-screen black boot screen */
    boot_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(boot_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(boot_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(boot_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_scrollbar_mode(boot_screen, LV_SCROLLBAR_MODE_OFF);

    /* Arc progress bar (412x412, centered) */
    progress_arc = lv_arc_create(boot_screen);
    lv_obj_set_size(progress_arc, 412, 412);
    lv_obj_align(progress_arc, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_bg_angles(progress_arc, 0, 360);
    lv_arc_set_rotation(progress_arc, 270); /* Start from top */
    lv_arc_set_range(progress_arc, 0, 100);
    lv_arc_set_value(progress_arc, 0);
    lv_arc_set_mode(progress_arc, LV_ARC_MODE_SYMMETRICAL);
    lv_obj_clear_flag(progress_arc, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_style_arc_color(progress_arc, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_arc_width(progress_arc, 15, LV_PART_MAIN);
    lv_obj_set_style_arc_color(progress_arc, lv_color_hex(0xA1D42A), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(progress_arc, 15, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(progress_arc, LV_OPA_TRANSP, LV_PART_KNOB);

    /* Boot intro image (inside the arc) */
    intro_img = lv_img_create(boot_screen);
    lv_obj_align(intro_img, LV_ALIGN_CENTER, 0, 0);

    /* Status label (top center) */
    status_label = lv_label_create(boot_screen);
    lv_label_set_text(status_label, "Starting...");
    lv_obj_set_style_text_color(status_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, -150);

    /* Percentage label (bottom center) */
    percent_label = lv_label_create(boot_screen);
    lv_label_set_text(percent_label, "0%");
    lv_obj_set_style_text_color(percent_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(percent_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(percent_label, LV_ALIGN_CENTER, 0, 140);

    /* Error image (hidden initially) */
    error_img = lv_img_create(boot_screen);
    lv_obj_add_flag(error_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(error_img, LV_ALIGN_CENTER, 0, 0);

    /* Countdown label (hidden initially) */
    countdown_label = lv_label_create(boot_screen);
    lv_obj_add_flag(countdown_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(countdown_label, lv_color_hex(0xFF5555), 0);
    lv_obj_set_style_text_align(countdown_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(countdown_label, LV_ALIGN_CENTER, 0, 140);

    /* Load boot screen */
    lv_disp_load_scr(boot_screen);
    in_error_mode = false;

    lvgl_port_unlock();

    ESP_LOGI(TAG, "Boot animation initialized");
}

void boot_anim_start_intro(emoji_anim_type_t type, int max_frames, uint32_t interval_ms) {
    if (in_error_mode || intro_img == NULL) {
        return;
    }

    int available_frames = emoji_get_frame_count(type);
    if (available_frames <= 0) {
        ESP_LOGW(TAG, "No intro frames available for type: %s", emoji_type_name(type));
        return;
    }

    intro_type = type;
    intro_frame_count = available_frames;
    if (max_frames > 0 && max_frames < intro_frame_count) {
        intro_frame_count = max_frames;
    }
    intro_frame = 0;

    lv_img_dsc_t *img = emoji_get_image(intro_type, intro_frame);
    if (img == NULL) {
        ESP_LOGW(TAG, "Failed to get first intro frame for type: %s", emoji_type_name(type));
        return;
    }

    lvgl_port_lock(0);
    lv_img_set_src(intro_img, img);
    lvgl_port_unlock();

    if (intro_timer != NULL) {
        lv_timer_set_period(intro_timer, interval_ms);
        lv_timer_resume(intro_timer);
    } else {
        intro_timer = lv_timer_create(boot_anim_intro_timer_cb, interval_ms, NULL);
    }

    ESP_LOGI(TAG, "Boot intro started: %s (%d frames @ %lums)", emoji_type_name(type), intro_frame_count,
             (unsigned long)interval_ms);
}

/* ------------------------------------------------------------------ */
/* Public: Set progress                                                */
/* ------------------------------------------------------------------ */

void boot_anim_set_progress(int percent) {
    if (in_error_mode)
        return;
    if (!progress_arc || !percent_label)
        return;

    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;

    ESP_LOGI(TAG, "Progress: %d%%", percent);

    lvgl_port_lock(0);
    lv_arc_set_value(progress_arc, percent);
    static char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", percent);
    lv_label_set_text(percent_label, buf);
    lvgl_port_unlock();
}

/* ------------------------------------------------------------------ */
/* Public: Set status text                                             */
/* ------------------------------------------------------------------ */

void boot_anim_set_text(const char *text) {
    if (in_error_mode)
        return;
    if (status_label && text) {
        lvgl_port_lock(0);
        lv_label_set_text(status_label, text);
        lvgl_port_unlock();
    }
}

/* ------------------------------------------------------------------ */
/* Public: Show error and countdown to reboot                          */
/* ------------------------------------------------------------------ */

void boot_anim_show_error(const char *error_msg) {
    if (in_error_mode)
        return;
    in_error_mode = true;

    ESP_LOGE(TAG, "Boot error: %s", error_msg);
    boot_anim_stop_intro();

    /* Hide progress elements */
    lv_obj_add_flag(progress_arc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(percent_label, LV_OBJ_FLAG_HIDDEN);

    /* Show error message in red */
    if (status_label && error_msg) {
        lv_label_set_text(status_label, error_msg);
        lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF5555), 0);
    }

    /* Load sad emoji image */
    if (error_img) {
        lv_img_dsc_t *sad_img = emoji_get_image(EMOJI_ANIM_ERROR, 0); /* error face */
        if (sad_img) {
            lv_img_set_src(error_img, sad_img);
            lv_obj_clear_flag(error_img, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Show countdown label */
    if (countdown_label) {
        lv_label_set_text(countdown_label, "Reboot in 10s");
        lv_obj_clear_flag(countdown_label, LV_OBJ_FLAG_HIDDEN);
    }

    /* Start countdown task */
    countdown_seconds = 10;
    xTaskCreate(boot_anim_countdown_task, "boot_countdown", 2048, NULL, 5, NULL);
}

/* ------------------------------------------------------------------ */
/* Private: Countdown task                                             */
/* ------------------------------------------------------------------ */

static void boot_anim_countdown_task(void *param) {
    (void)param;

    while (countdown_seconds > 0) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        countdown_seconds--;

        if (countdown_label) {
            static char buf[32];
            if (countdown_seconds > 0) {
                snprintf(buf, sizeof(buf), "Reboot in %ds", countdown_seconds);
            } else {
                snprintf(buf, sizeof(buf), "Rebooting...");
            }
            lv_label_set_text(countdown_label, buf);
        }

        if (countdown_seconds == 0) {
            break;
        }
    }

    /* Reboot */
    ESP_LOGW(TAG, "Rebooting due to boot error...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    vTaskDelete(NULL);
    /* Should not reach here */
}

static void boot_anim_intro_timer_cb(lv_timer_t *timer) {
    (void)timer;

    if (in_error_mode || intro_img == NULL || intro_type == EMOJI_ANIM_NONE || intro_frame_count <= 0) {
        return;
    }

    intro_frame = (intro_frame + 1) % intro_frame_count;
    lv_img_dsc_t *img = emoji_get_image(intro_type, intro_frame);
    if (img != NULL) {
        lv_img_set_src(intro_img, img);
    }
}

static void boot_anim_stop_intro(void) {
    if (intro_timer != NULL) {
        lv_timer_del(intro_timer);
        intro_timer = NULL;
    }

    intro_type = EMOJI_ANIM_NONE;
    intro_frame = 0;
    intro_frame_count = 0;
}

/* ------------------------------------------------------------------ */
/* Public: Finish boot animation                                       */
/* ------------------------------------------------------------------ */

void boot_anim_finish(void) {
    if (in_error_mode)
        return;

    /* Brief delay to show 100% */
    vTaskDelay(pdMS_TO_TICKS(300));
    boot_anim_stop_intro();

    /* Clear child pointers - do NOT delete the screen here.
     * The caller must load a new screen first, then call
     * lv_obj_del(boot_anim_get_screen()) to safely free it. */
    intro_img = NULL;
    progress_arc = NULL;
    percent_label = NULL;
    status_label = NULL;
    error_img = NULL;
    countdown_label = NULL;

    ESP_LOGI(TAG, "Boot animation finished");
}

lv_obj_t *boot_anim_get_screen(void) {
    return boot_screen;
}
