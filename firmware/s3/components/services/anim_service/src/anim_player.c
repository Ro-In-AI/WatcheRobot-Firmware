/**
 * @file anim_player.c
 * @brief Animation player implementation with 30fps playback
 */

#include "anim_player.h"
#include "anim_meta.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#ifdef CONFIG_WATCHER_ANIM_FPS
#define DEFAULT_FPS CONFIG_WATCHER_ANIM_FPS
#else
#define DEFAULT_FPS 30
#endif

#ifdef CONFIG_WATCHER_ANIM_CACHE_RGB565
#define DEFAULT_USE_CACHE true
#else
#define DEFAULT_USE_CACHE false
#endif

#ifdef CONFIG_WATCHER_ANIM_DEBUG_PERF
#define DEBUG_PERF_ENABLED true
#else
#define DEBUG_PERF_ENABLED false
#endif

#define TAG "ANIM_PLAYER"

/* Animation state */
static lv_obj_t *g_img_obj = NULL;
static lv_timer_t *g_timer = NULL;
static emoji_anim_type_t g_current_type = EMOJI_ANIM_NONE;
static int g_current_frame = 0;
static uint32_t g_interval_ms = EMOJI_ANIM_INTERVAL_MS;
static bool g_use_cache = DEFAULT_USE_CACHE;
/* Keep a persistent descriptor for cached RGB565 frames.
 * lv_img_set_src() keeps the descriptor pointer, so stack descriptors are unsafe. */
static lv_img_dsc_t g_cached_img_dsc = {0};

/* Performance tracking */
static int64_t g_frame_start_us = 0;
static int g_frames_displayed = 0;

static void emoji_timer_callback(lv_timer_t *timer)
{
    (void)timer;
    int64_t start_us = esp_timer_get_time();

    if (g_current_type == EMOJI_ANIM_NONE || g_img_obj == NULL) {
        return;
    }

    /* Try cached RGB565 frames first */
    if (g_use_cache && anim_cache_is_type_loaded(g_current_type)) {
        int frame_count = anim_cache_get_frame_count();
        if (frame_count <= 0) {
            ESP_LOGW(TAG, "No cached frames for type %d", g_current_type);
            emoji_anim_stop();
            return;
        }

        /* Advance to next frame */
        g_current_frame = (g_current_frame + 1) % frame_count;

        /* Get cached RGB565 frame */
        anim_cached_frame_t *cached = anim_cache_get_frame(g_current_type, g_current_frame);
        if (cached != NULL && cached->rgb565_data != NULL) {
            /* Update persistent descriptor for RGB565 data */
            g_cached_img_dsc.header.always_zero = 0;
            g_cached_img_dsc.header.w = cached->width;
            g_cached_img_dsc.header.h = cached->height;
            g_cached_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
            g_cached_img_dsc.data_size = cached->data_size;
            g_cached_img_dsc.data = cached->rgb565_data;
            lv_img_set_src(g_img_obj, &g_cached_img_dsc);

            g_frames_displayed++;

            /* Log performance every 30 frames if debug enabled */
            if (DEBUG_PERF_ENABLED && g_frames_displayed % 30 == 0) {
                int64_t elapsed_us = esp_timer_get_time() - g_frame_start_us;
                int actual_fps = (g_frames_displayed * 1000000) / elapsed_us;
                int64_t frame_time_us = esp_timer_get_time() - start_us;
                ESP_LOGI(TAG, "FPS: %d, frame switch: %lld us", actual_fps, frame_time_us);
            }
        }
    } else {
        /* Fallback to PNG decoding (slower) */
        int frame_count = emoji_get_frame_count(g_current_type);
        if (frame_count <= 0) {
            ESP_LOGW(TAG, "No frames for type %d", g_current_type);
            emoji_anim_stop();
            return;
        }

        /* Advance to next frame */
        g_current_frame = (g_current_frame + 1) % frame_count;

        /* Get PNG image descriptor */
        lv_img_dsc_t *img = emoji_get_image(g_current_type, g_current_frame);
        if (img != NULL) {
            lv_img_set_src(g_img_obj, img);
        }
    }
}

int emoji_anim_init(lv_obj_t *img_obj)
{
    ESP_LOGI(TAG, "emoji_anim_init called");

    if (img_obj == NULL) {
        ESP_LOGE(TAG, "Invalid image object");
        return -1;
    }

    g_img_obj = img_obj;
    g_current_type = EMOJI_ANIM_NONE;
    g_current_frame = 1;
    g_frames_displayed = 0;
    g_frame_start_us = esp_timer_get_time();

    ESP_LOGI(TAG, "Calling anim_meta_init...");
    /* Initialize metadata system */
    if (anim_meta_init() != 0) {
        ESP_LOGW(TAG, "Metadata init failed, using defaults");
    }
    ESP_LOGI(TAG, "anim_meta_init done");

    ESP_LOGI(TAG, "Calling anim_cache_init...");
    /* Initialize RGB565 cache */
    if (anim_cache_init() != 0) {
        ESP_LOGW(TAG, "Cache init failed, falling back to PNG decode");
        g_use_cache = false;
    }
    ESP_LOGI(TAG, "anim_cache_init done");

    /* Get default FPS from metadata */
    int fps = anim_meta_get_default_fps();
    g_interval_ms = 1000 / fps;

    ESP_LOGI(TAG, "Animation system initialized (FPS: %d, interval: %lums)",
             fps, (unsigned long)g_interval_ms);
    return 0;
}

int emoji_anim_start(emoji_anim_type_t type)
{
    if (g_img_obj == NULL) {
        ESP_LOGE(TAG, "Animation not initialized");
        return -1;
    }

    if (type < 0 || type >= EMOJI_ANIM_COUNT) {
        ESP_LOGE(TAG, "Invalid emoji type: %d", type);
        return -1;
    }

    /* If same type is already playing, no need to restart */
    if (g_current_type == type && g_timer != NULL) {
        ESP_LOGD(TAG, "Animation %s already playing", emoji_type_name(type));
        return 0;
    }

    /* Check if PNG images are available first */
    if (emoji_get_frame_count(type) <= 0) {
        ESP_LOGW(TAG, "No frames available for type: %s", emoji_type_name(type));
        return -1;
    }

    /* Set new type and reset frame */
    g_current_type = type;
    g_current_frame = 0;
    g_frames_displayed = 0;
    g_frame_start_us = esp_timer_get_time();

    /* Get FPS for this animation type */
    int fps = anim_meta_get_fps(type);
    g_interval_ms = 1000 / fps;

    /* Show first frame IMMEDIATELY from PNG (before cache load) */
    lv_img_dsc_t *first_img = emoji_get_image(type, 0);
    if (first_img != NULL) {
        lv_img_set_src(g_img_obj, first_img);
    }

    /* Load and cache RGB565 frames if cache is enabled and not already cached */
    if (g_use_cache && !anim_cache_is_type_loaded(type)) {
        int64_t load_start = esp_timer_get_time();

        if (anim_cache_load_type(type) != 0) {
            ESP_LOGW(TAG, "Failed to cache type %s, using PNG decode", emoji_type_name(type));
            g_use_cache = false;  /* Disable cache for this session */
        } else {
            int64_t load_time_ms = (esp_timer_get_time() - load_start) / 1000;
            ESP_LOGI(TAG, "Type %s loaded in %lld ms", emoji_type_name(type), load_time_ms);
        }
    }

    /* Get frame count */
    int frame_count;
    if (g_use_cache && anim_cache_is_type_loaded(type)) {
        frame_count = anim_cache_get_frame_count();
    } else {
        frame_count = emoji_get_frame_count(type);
    }

    /* Reuse or create timer for animation */
    if (frame_count > 1) {
        if (g_timer != NULL) {
            /* Reuse existing timer */
            lv_timer_reset(g_timer);
            lv_timer_set_period(g_timer, g_interval_ms);
            lv_timer_resume(g_timer);
        } else {
            /* Create new timer */
            g_timer = lv_timer_create(emoji_timer_callback, g_interval_ms, NULL);
            if (g_timer == NULL) {
                ESP_LOGE(TAG, "Failed to create timer");
                return -1;
            }
        }
    } else {
        /* Single frame - pause timer if exists */
        if (g_timer != NULL) {
            lv_timer_pause(g_timer);
        }
    }

    ESP_LOGI(TAG, "Started animation: %s (%d frames @ %d fps)",
             emoji_type_name(type), frame_count, fps);
    return 0;
}

void emoji_anim_stop(void)
{
    /* Pause timer instead of deleting */
    if (g_timer != NULL) {
        lv_timer_pause(g_timer);
    }
    g_current_type = EMOJI_ANIM_NONE;
    g_current_frame = 0;
}

bool emoji_anim_is_running(void)
{
    return g_timer != NULL && g_current_type != EMOJI_ANIM_NONE;
}

emoji_anim_type_t emoji_anim_get_type(void)
{
    return g_current_type;
}

void emoji_anim_set_interval(uint32_t interval_ms)
{
    g_interval_ms = interval_ms;
    if (g_timer != NULL) {
        lv_timer_set_period(g_timer, interval_ms);
    }
}

int emoji_anim_show_static(emoji_anim_type_t type, int frame)
{
    if (g_img_obj == NULL) {
        ESP_LOGE(TAG, "Animation not initialized");
        return -1;
    }

    /* Stop any running animation */
    emoji_anim_stop();

    if (type < 0 || type >= EMOJI_ANIM_COUNT) {
        ESP_LOGE(TAG, "Invalid emoji type: %d", type);
        return -1;
    }

    /* Try cache first */
    if (g_use_cache) {
        if (!anim_cache_is_type_loaded(type)) {
            anim_cache_load_type(type);
        }

        anim_cached_frame_t *cached = anim_cache_get_frame(type, frame);
        if (cached != NULL && cached->rgb565_data != NULL) {
            g_cached_img_dsc.header.always_zero = 0;
            g_cached_img_dsc.header.w = cached->width;
            g_cached_img_dsc.header.h = cached->height;
            g_cached_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
            g_cached_img_dsc.data_size = cached->data_size;
            g_cached_img_dsc.data = cached->rgb565_data;
            lv_img_set_src(g_img_obj, &g_cached_img_dsc);
            g_current_type = type;
            g_current_frame = frame;
            return 0;
        }
    }

    /* Fallback to PNG */
    lv_img_dsc_t *img = emoji_get_image(type, frame);
    if (img == NULL) {
        img = emoji_get_image(type, 0);
        if (img == NULL) {
            ESP_LOGE(TAG, "No image available for type: %s", emoji_type_name(type));
            return -1;
        }
    }

    lv_img_set_src(g_img_obj, img);
    g_current_type = type;
    g_current_frame = frame;

    ESP_LOGI(TAG, "Showing static: %s frame %d", emoji_type_name(type), frame);
    return 0;
}

int emoji_anim_prefetch_type(emoji_anim_type_t type)
{
    if (type < 0 || type >= EMOJI_ANIM_COUNT) {
        return -1;
    }

    if (!g_use_cache) {
        return 0;  /* No-op if cache disabled */
    }

    /* Don't prefetch if already cached */
    if (anim_cache_is_type_loaded(type)) {
        return 0;
    }

    ESP_LOGI(TAG, "Prefetching type: %s", emoji_type_name(type));
    return anim_cache_load_type(type);
}

int emoji_anim_get_fps(void)
{
    return 1000 / g_interval_ms;
}

void emoji_anim_set_fps(int fps)
{
    if (fps < 1) fps = 1;
    if (fps > 60) fps = 60;

    g_interval_ms = 1000 / fps;
    if (g_timer != NULL) {
        lv_timer_set_period(g_timer, g_interval_ms);
    }

    ESP_LOGI(TAG, "FPS set to %d (interval: %lums)", fps, (unsigned long)g_interval_ms);
}
