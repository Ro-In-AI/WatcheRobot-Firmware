/**
 * @file anim_player.c
 * @brief Animation player with warm-cache previews and async hot-cache switching.
 */

#include "anim_player.h"
#include "anim_meta.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>

#define TAG "ANIM_PLAYER"

#ifdef CONFIG_WATCHER_ANIM_FPS
#define DEFAULT_FPS CONFIG_WATCHER_ANIM_FPS
#else
#define DEFAULT_FPS 10
#endif

#ifdef CONFIG_WATCHER_ANIM_CACHE_RGB565
#define DEFAULT_USE_CACHE true
#else
#define DEFAULT_USE_CACHE false
#endif

#ifdef CONFIG_WATCHER_ANIM_DEBUG_PERF
#define ANIM_DEBUG_PERF_ENABLED 1
#else
#define ANIM_DEBUG_PERF_ENABLED 0
#endif

#define ANIM_SOURCE_FRAME_SIZE 206
#define ANIM_SOURCE_FRAME_PIVOT (ANIM_SOURCE_FRAME_SIZE / 2)
#define ANIM_DISPLAY_ZOOM_2X (LV_IMG_ZOOM_NONE * 2)
#define ANIM_PREVIEW_ONLY_INTERNAL_HEAP_THRESHOLD (40U * 1024U)
#define ANIM_PREVIEW_ONLY_INTERNAL_LARGEST_BLOCK_THRESHOLD (24U * 1024U)
#define ANIM_PREVIEW_ONLY_DMA_HEAP_THRESHOLD (24U * 1024U)
#define ANIM_PREVIEW_ONLY_DMA_LARGEST_BLOCK_THRESHOLD (16U * 1024U)

typedef enum {
    ANIM_PLAYER_IDLE = 0,
    ANIM_PLAYER_PLAYING,
    ANIM_PLAYER_SWITCH_PREPARING,
    ANIM_PLAYER_SWITCH_COMMITTING,
} anim_player_state_t;

typedef struct {
    emoji_anim_type_t type;
    uint32_t generation_id;
} anim_build_request_t;

typedef struct {
    emoji_anim_type_t type;
    uint32_t generation_id;
    int status;
    uint32_t build_time_ms;
} anim_build_result_t;

static lv_obj_t *g_front_img = NULL;
static lv_obj_t *g_back_img = NULL;
static lv_timer_t *g_frame_timer = NULL;
static lv_timer_t *g_service_timer = NULL;
static QueueHandle_t g_request_queue = NULL;
static QueueHandle_t g_result_queue = NULL;
static TaskHandle_t g_worker_task = NULL;

static anim_player_state_t g_state = ANIM_PLAYER_IDLE;
static emoji_anim_type_t g_current_type = EMOJI_ANIM_NONE;
static emoji_anim_type_t g_requested_type = EMOJI_ANIM_NONE;
static int g_current_frame = 0;
static uint32_t g_interval_ms = EMOJI_ANIM_INTERVAL_MS;
static bool g_use_cache = DEFAULT_USE_CACHE;
static volatile uint32_t g_latest_generation = 0;
static int64_t g_anim_start_us = 0;

typedef struct {
    size_t free_internal;
    size_t largest_internal;
    size_t free_dma;
    size_t largest_dma;
} anim_heap_snapshot_t;

typedef struct {
    bool active;
    emoji_anim_type_t from_type;
    emoji_anim_type_t to_type;
    uint32_t generation_id;
    int64_t request_us;
    anim_heap_snapshot_t heap_snapshot;
} anim_switch_diag_t;

typedef struct {
    bool active;
    emoji_anim_type_t type;
    int64_t window_start_us;
    uint32_t frame_switches;
} anim_fps_diag_t;

static anim_switch_diag_t g_switch_diag = {0};
static anim_fps_diag_t g_fps_diag = {0};

static void anim_worker_task(void *param);
static void anim_frame_timer_cb(lv_timer_t *timer);
static void anim_service_timer_cb(lv_timer_t *timer);
static void hide_preview_layer(void);
static int get_playback_frame_count(emoji_anim_type_t type);
static const lv_img_dsc_t *get_playback_frame(emoji_anim_type_t type, int frame_index);
static void cancel_transition_animations(void);
static void log_preview_only_state(const char *reason, emoji_anim_type_t requested_type);
static bool can_build_after_releasing_active(const anim_hot_build_budget_t *budget);
static void show_pending_preview(emoji_anim_type_t type, const lv_img_dsc_t *preview);
static int activate_hot_cache(emoji_anim_type_t type, uint32_t generation_id);
static void recover_after_switch_failure(emoji_anim_type_t failed_type,
                                         uint32_t generation_id,
                                         uint32_t build_time_ms,
                                         const char *reason);

static anim_heap_snapshot_t capture_heap_snapshot(void) {
    anim_heap_snapshot_t snapshot = {
        .free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        .largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        .free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
        .largest_dma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
    };
    return snapshot;
}

static const char *anim_type_name_or_none(emoji_anim_type_t type) {
    return type == EMOJI_ANIM_NONE ? "none" : emoji_type_name(type);
}

static emoji_anim_type_t anim_displayed_type(void) {
    return g_current_type;
}

static emoji_anim_type_t anim_active_type(void) {
    return anim_hot_get_active_type();
}

static void clear_switch_diag(void) {
    memset(&g_switch_diag, 0, sizeof(g_switch_diag));
}

static void begin_switch_diag(emoji_anim_type_t from_type,
                              emoji_anim_type_t to_type,
                              uint32_t generation_id,
                              const anim_heap_snapshot_t *snapshot) {
    if (!ANIM_DEBUG_PERF_ENABLED) {
        return;
    }

    g_switch_diag.active = true;
    g_switch_diag.from_type = from_type;
    g_switch_diag.to_type = to_type;
    g_switch_diag.generation_id = generation_id;
    g_switch_diag.request_us = esp_timer_get_time();
    if (snapshot != NULL) {
        g_switch_diag.heap_snapshot = *snapshot;
    } else {
        memset(&g_switch_diag.heap_snapshot, 0, sizeof(g_switch_diag.heap_snapshot));
    }
}

static void log_switch_request(const char *path,
                               emoji_anim_type_t from_type,
                               emoji_anim_type_t to_type,
                               uint32_t generation_id,
                               const anim_heap_snapshot_t *snapshot) {
    if (!ANIM_DEBUG_PERF_ENABLED || snapshot == NULL) {
        return;
    }

    ESP_LOGI(TAG,
             "Switch request gen=%lu %s -> %s path=%s heap internal=%u/%u dma=%u/%u",
             (unsigned long)generation_id,
             anim_type_name_or_none(from_type),
             anim_type_name_or_none(to_type),
             path,
             (unsigned)snapshot->free_internal,
             (unsigned)snapshot->largest_internal,
             (unsigned)snapshot->free_dma,
             (unsigned)snapshot->largest_dma);
}

static void log_switch_success(uint32_t build_time_ms, uint32_t commit_time_ms) {
    if (!ANIM_DEBUG_PERF_ENABLED || !g_switch_diag.active) {
        clear_switch_diag();
        return;
    }

    int64_t now_us = esp_timer_get_time();
    uint32_t total_time_ms = g_switch_diag.request_us > 0 ? (uint32_t)((now_us - g_switch_diag.request_us) / 1000ULL) : 0;
    ESP_LOGI(TAG,
             "Switch committed gen=%lu %s -> %s build_ms=%lu commit_ms=%lu total_ms=%lu active=%s displayed=%s heap internal=%u/%u dma=%u/%u",
             (unsigned long)g_switch_diag.generation_id,
             anim_type_name_or_none(g_switch_diag.from_type),
             anim_type_name_or_none(g_switch_diag.to_type),
             (unsigned long)build_time_ms,
             (unsigned long)commit_time_ms,
             (unsigned long)total_time_ms,
             anim_type_name_or_none(anim_active_type()),
             anim_type_name_or_none(anim_displayed_type()),
             (unsigned)g_switch_diag.heap_snapshot.free_internal,
             (unsigned)g_switch_diag.heap_snapshot.largest_internal,
             (unsigned)g_switch_diag.heap_snapshot.free_dma,
             (unsigned)g_switch_diag.heap_snapshot.largest_dma);
    clear_switch_diag();
}

static void log_switch_failure(emoji_anim_type_t failed_type,
                               uint32_t generation_id,
                               uint32_t build_time_ms,
                               const char *reason,
                               const char *fallback) {
    int64_t now_us = esp_timer_get_time();
    uint32_t total_time_ms = 0;
    emoji_anim_type_t from_type = g_current_type;
    anim_heap_snapshot_t snapshot = {0};

    if (g_switch_diag.active && g_switch_diag.generation_id == generation_id && g_switch_diag.to_type == failed_type) {
        from_type = g_switch_diag.from_type;
        snapshot = g_switch_diag.heap_snapshot;
        if (g_switch_diag.request_us > 0) {
            total_time_ms = (uint32_t)((now_us - g_switch_diag.request_us) / 1000ULL);
        }
    }

    ESP_LOGW(TAG,
             "Switch failed gen=%lu %s -> %s reason=%s fallback=%s requested=%s active=%s displayed=%s build_ms=%lu total_ms=%lu heap internal=%u/%u dma=%u/%u",
             (unsigned long)generation_id,
             anim_type_name_or_none(from_type),
             anim_type_name_or_none(failed_type),
             reason,
             fallback,
             anim_type_name_or_none(failed_type),
             anim_type_name_or_none(anim_active_type()),
             anim_type_name_or_none(anim_displayed_type()),
             (unsigned long)build_time_ms,
             (unsigned long)total_time_ms,
             (unsigned)snapshot.free_internal,
             (unsigned)snapshot.largest_internal,
             (unsigned)snapshot.free_dma,
             (unsigned)snapshot.largest_dma);
    clear_switch_diag();
}

static void log_preview_only_state(const char *reason, emoji_anim_type_t requested_type) {
    if (!ANIM_DEBUG_PERF_ENABLED) {
        return;
    }

    ESP_LOGI(TAG,
             "Preview-only active requested=%s active=%s displayed=%s reason=%s target_fps=%d actual_fps=0",
             anim_type_name_or_none(requested_type),
             anim_type_name_or_none(anim_active_type()),
             anim_type_name_or_none(anim_displayed_type()),
             reason,
             emoji_anim_get_fps());
}

static bool can_build_after_releasing_active(const anim_hot_build_budget_t *budget) {
    if (budget == NULL || budget->estimated_bytes == 0 || budget->active_bytes == 0) {
        return false;
    }

    return budget->estimated_bytes <= WATCHER_ANIM_HOT_BUDGET_BYTES &&
           budget->free_spiram + budget->active_bytes > budget->estimated_bytes + WATCHER_ANIM_SAFETY_MARGIN_BYTES;
}

static void reset_fps_diag(emoji_anim_type_t type) {
    if (!ANIM_DEBUG_PERF_ENABLED) {
        return;
    }

    g_fps_diag.active = true;
    g_fps_diag.type = type;
    g_fps_diag.window_start_us = esp_timer_get_time();
    g_fps_diag.frame_switches = 0;
}

static void clear_fps_diag(void) {
    memset(&g_fps_diag, 0, sizeof(g_fps_diag));
}

static void note_frame_switch(emoji_anim_type_t type) {
    if (!ANIM_DEBUG_PERF_ENABLED) {
        return;
    }

    int64_t now_us = esp_timer_get_time();
    if (!g_fps_diag.active || g_fps_diag.type != type || g_fps_diag.window_start_us == 0) {
        reset_fps_diag(type);
    }

    g_fps_diag.frame_switches++;
    int64_t elapsed_us = now_us - g_fps_diag.window_start_us;
    if (elapsed_us < 1000000LL || g_fps_diag.frame_switches == 0) {
        return;
    }

    g_fps_diag.window_start_us = now_us;
    g_fps_diag.frame_switches = 0;
}

static bool should_use_preview_only_mode(const anim_heap_snapshot_t *snapshot) {
    if (snapshot == NULL) {
        return false;
    }

    bool internal_low = (snapshot->free_internal > 0 && snapshot->free_internal < ANIM_PREVIEW_ONLY_INTERNAL_HEAP_THRESHOLD) ||
                        (snapshot->largest_internal > 0 &&
                         snapshot->largest_internal < ANIM_PREVIEW_ONLY_INTERNAL_LARGEST_BLOCK_THRESHOLD);
    bool dma_low = (snapshot->free_dma > 0 && snapshot->free_dma < ANIM_PREVIEW_ONLY_DMA_HEAP_THRESHOLD) ||
                   (snapshot->largest_dma > 0 && snapshot->largest_dma < ANIM_PREVIEW_ONLY_DMA_LARGEST_BLOCK_THRESHOLD);
    return internal_low || dma_low;
}

static void activate_preview_only(emoji_anim_type_t type, const lv_img_dsc_t *preview) {
    if (g_front_img == NULL || preview == NULL) {
        return;
    }

    int frame_count = emoji_load_type(type);
    if (frame_count <= 0) {
        frame_count = emoji_get_frame_count(type);
    }

    lv_img_set_src(g_front_img, preview);
    lv_obj_set_style_opa(g_front_img, LV_OPA_COVER, 0);
    hide_preview_layer();

    g_current_type = type;
    g_requested_type = type;
    g_current_frame = 0;
    g_state = ANIM_PLAYER_PLAYING;
    g_anim_start_us = esp_timer_get_time();
    reset_fps_diag(type);

    if (g_frame_timer != NULL) {
        if (frame_count > 1) {
            lv_timer_resume(g_frame_timer);
        } else {
            lv_timer_pause(g_frame_timer);
        }
    }
}

static void show_pending_preview(emoji_anim_type_t type, const lv_img_dsc_t *preview) {
    if (g_front_img == NULL || preview == NULL) {
        return;
    }

    lv_img_set_src(g_front_img, preview);
    lv_obj_set_style_opa(g_front_img, LV_OPA_COVER, 0);
    hide_preview_layer();

    g_current_type = type;
    g_requested_type = type;
    g_current_frame = 0;
    g_anim_start_us = esp_timer_get_time();
    clear_fps_diag();
}

static void set_obj_opa(void *obj, int32_t value) {
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void configure_anim_layer(lv_obj_t *img_obj) {
    if (img_obj == NULL) {
        return;
    }

    /* Keep the image object's layout box stable before frames are assigned.
     * Otherwise aligning a zero-sized lv_img and then setting src later shifts
     * the visual center down/right once LVGL updates the intrinsic image size. */
    lv_obj_set_size(img_obj, ANIM_SOURCE_FRAME_SIZE, ANIM_SOURCE_FRAME_SIZE);
    lv_img_set_pivot(img_obj, ANIM_SOURCE_FRAME_PIVOT, ANIM_SOURCE_FRAME_PIVOT);
    lv_img_set_zoom(img_obj, ANIM_DISPLAY_ZOOM_2X);
    lv_img_set_antialias(img_obj, false);
}

static void cancel_transition_animations(void) {
    if (g_back_img != NULL) {
        lv_anim_del(g_back_img, set_obj_opa);
    }
    if (g_front_img != NULL) {
        lv_anim_del(g_front_img, set_obj_opa);
    }
}

static void hide_preview_layer(void) {
    cancel_transition_animations();

    if (g_back_img == NULL) {
        if (g_front_img != NULL) {
            lv_obj_set_style_opa(g_front_img, LV_OPA_COVER, 0);
        }
        return;
    }
    lv_obj_add_flag(g_back_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(g_back_img, LV_OPA_TRANSP, 0);
    if (g_front_img != NULL) {
        lv_obj_set_style_opa(g_front_img, LV_OPA_COVER, 0);
    }
}

static void start_preview_fade(const lv_img_dsc_t *preview) {
    if (g_front_img == NULL || preview == NULL) {
        return;
    }

    if (g_back_img == NULL || g_current_type == EMOJI_ANIM_NONE || !anim_hot_is_active_type(g_current_type)) {
        lv_img_set_src(g_front_img, preview);
        lv_obj_set_style_opa(g_front_img, LV_OPA_COVER, 0);
        hide_preview_layer();
        return;
    }

    lv_anim_del(g_back_img, set_obj_opa);
    lv_anim_del(g_front_img, set_obj_opa);

    lv_obj_set_style_opa(g_front_img, LV_OPA_COVER, 0);
    lv_img_set_src(g_back_img, preview);
    lv_obj_clear_flag(g_back_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(g_back_img, LV_OPA_TRANSP, 0);

    if (WATCHER_ANIM_SWITCH_FADE_MS <= 0) {
        lv_obj_set_style_opa(g_back_img, LV_OPA_COVER, 0);
        lv_obj_set_style_opa(g_front_img, LV_OPA_TRANSP, 0);
        return;
    }

    lv_anim_t fade_in;
    lv_anim_init(&fade_in);
    lv_anim_set_var(&fade_in, g_back_img);
    lv_anim_set_values(&fade_in, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&fade_in, WATCHER_ANIM_SWITCH_FADE_MS);
    lv_anim_set_exec_cb(&fade_in, set_obj_opa);
    lv_anim_start(&fade_in);

    lv_anim_t fade_out;
    lv_anim_init(&fade_out);
    lv_anim_set_var(&fade_out, g_front_img);
    lv_anim_set_values(&fade_out, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&fade_out, WATCHER_ANIM_SWITCH_FADE_MS);
    lv_anim_set_exec_cb(&fade_out, set_obj_opa);
    lv_anim_start(&fade_out);
}

static void set_playback_clock(emoji_anim_type_t type) {
    const anim_catalog_type_info_t *info = anim_catalog_get_type_info(type);
    int fps = (info != NULL && info->available && info->fps > 0) ? info->fps : anim_meta_get_fps(type);
    if (fps <= 0) {
        fps = DEFAULT_FPS;
    }
    g_interval_ms = 1000U / (uint32_t)fps;
    if (g_interval_ms == 0) {
        g_interval_ms = 1;
    }
    if (g_frame_timer != NULL) {
        lv_timer_set_period(g_frame_timer, g_interval_ms);
    }
}

static void resume_frame_timer_if_needed(emoji_anim_type_t type) {
    int frame_count = get_playback_frame_count(type);
    if (g_frame_timer == NULL) {
        return;
    }
    if (frame_count > 1) {
        lv_timer_resume(g_frame_timer);
    } else {
        lv_timer_pause(g_frame_timer);
    }
}

static int activate_hot_cache(emoji_anim_type_t type, uint32_t generation_id) {
    if (anim_hot_commit_prepared(type, generation_id) != 0) {
        return -1;
    }

    anim_cached_frame_t *frame0 = anim_hot_get_frame(type, 0);
    if (frame0 != NULL && g_front_img != NULL) {
        lv_img_set_src(g_front_img, &frame0->img_dsc);
        lv_obj_set_style_opa(g_front_img, LV_OPA_COVER, 0);
    }

    hide_preview_layer();
    g_current_type = type;
    g_requested_type = type;
    g_current_frame = 0;
    g_state = ANIM_PLAYER_PLAYING;
    set_playback_clock(type);
    g_anim_start_us = esp_timer_get_time();
    reset_fps_diag(type);
    resume_frame_timer_if_needed(type);
    return 0;
}

static int submit_build_request(emoji_anim_type_t type, uint32_t generation_id) {
    anim_build_request_t request = {
        .type = type,
        .generation_id = generation_id,
    };

    if (g_request_queue == NULL) {
        return -1;
    }

    return xQueueOverwrite(g_request_queue, &request) == pdPASS ? 0 : -1;
}

static int ensure_worker_started(void) {
    if (g_request_queue == NULL) {
        g_request_queue = xQueueCreate(1, sizeof(anim_build_request_t));
        if (g_request_queue == NULL) {
            return -1;
        }
    }

    if (g_result_queue == NULL) {
        g_result_queue = xQueueCreate(1, sizeof(anim_build_result_t));
        if (g_result_queue == NULL) {
            return -1;
        }
    }

    if (g_worker_task == NULL) {
        BaseType_t rc = xTaskCreate(anim_worker_task, "anim_worker", 6144, NULL, 5, &g_worker_task);
        if (rc != pdPASS) {
            g_worker_task = NULL;
            return -1;
        }
    }

    return 0;
}

static const lv_img_dsc_t *get_preview_frame(emoji_anim_type_t type) {
    const lv_img_dsc_t *preview = anim_warm_get_first_frame(type);
    if (preview != NULL) {
        return preview;
    }

    if (emoji_load_type(type) > 0) {
        return emoji_get_image(type, 0);
    }

    return NULL;
}

static int get_playback_frame_count(emoji_anim_type_t type) {
    if (type == EMOJI_ANIM_NONE) {
        return 0;
    }

    if (anim_hot_is_active_type(type)) {
        return anim_hot_get_frame_count();
    }

    if (emoji_load_type(type) > 0) {
        return emoji_get_frame_count(type);
    }

    return 0;
}

static const lv_img_dsc_t *get_playback_frame(emoji_anim_type_t type, int frame_index) {
    if (type == EMOJI_ANIM_NONE || frame_index < 0) {
        return NULL;
    }

    if (anim_hot_is_active_type(type)) {
        anim_cached_frame_t *frame = anim_hot_get_frame(type, frame_index);
        return frame != NULL ? &frame->img_dsc : NULL;
    }

    return emoji_get_image(type, frame_index);
}

static void anim_worker_task(void *param) {
    (void)param;

    for (;;) {
        anim_build_request_t request = {0};
        if (xQueueReceive(g_request_queue, &request, portMAX_DELAY) != pdPASS) {
            continue;
        }

        int status = -1;
        int64_t build_started_us = 0;
        int64_t build_finished_us = 0;
        if (request.generation_id == g_latest_generation && request.type == g_requested_type) {
            build_started_us = esp_timer_get_time();
            status = anim_hot_build_type(request.type, request.generation_id);
            build_finished_us = esp_timer_get_time();
        }

        anim_build_result_t result = {
            .type = request.type,
            .generation_id = request.generation_id,
            .status = status,
            .build_time_ms = (build_started_us > 0 && build_finished_us >= build_started_us)
                                 ? (uint32_t)((build_finished_us - build_started_us) / 1000ULL)
                                 : 0,
        };
        xQueueOverwrite(g_result_queue, &result);
    }
}

static void anim_frame_timer_cb(lv_timer_t *timer) {
    (void)timer;

    if (g_state != ANIM_PLAYER_PLAYING || g_current_type == EMOJI_ANIM_NONE) {
        return;
    }

    int frame_count = get_playback_frame_count(g_current_type);
    if (frame_count <= 0) {
        return;
    }

    int64_t elapsed_us = esp_timer_get_time() - g_anim_start_us;
    uint32_t frame_interval_us = g_interval_ms * 1000U;
    int frame_index = frame_interval_us > 0 ? (int)(elapsed_us / (int64_t)frame_interval_us) : 0;

    if (anim_meta_should_loop(g_current_type)) {
        frame_index %= frame_count;
    } else if (frame_index >= frame_count) {
        frame_index = frame_count - 1;
    }

    if (frame_index == g_current_frame) {
        return;
    }

    const lv_img_dsc_t *frame = get_playback_frame(g_current_type, frame_index);
    if (frame != NULL && g_front_img != NULL) {
        lv_img_set_src(g_front_img, frame);
        g_current_frame = frame_index;
        note_frame_switch(g_current_type);
    }
}

static void recover_after_switch_failure(emoji_anim_type_t failed_type,
                                         uint32_t generation_id,
                                         uint32_t build_time_ms,
                                         const char *reason) {
    const lv_img_dsc_t *preview = get_preview_frame(failed_type);

    if (preview != NULL) {
        activate_preview_only(failed_type, preview);
        log_switch_failure(failed_type, generation_id, build_time_ms, reason, "preview-only");
        log_preview_only_state(reason, failed_type);
        return;
    }

    if (anim_hot_is_active_type(g_current_type) && g_current_type != EMOJI_ANIM_NONE) {
        hide_preview_layer();
        g_requested_type = g_current_type;
        g_state = ANIM_PLAYER_PLAYING;
        resume_frame_timer_if_needed(g_current_type);
        log_switch_failure(failed_type, generation_id, build_time_ms, reason, "keep-current");
        return;
    }

    if (g_frame_timer != NULL) {
        lv_timer_pause(g_frame_timer);
    }
    hide_preview_layer();
    g_state = ANIM_PLAYER_IDLE;
    g_current_type = EMOJI_ANIM_NONE;
    g_requested_type = EMOJI_ANIM_NONE;
    g_current_frame = 0;
    clear_fps_diag();
    log_switch_failure(failed_type, generation_id, build_time_ms, reason, "idle");
}

static void anim_service_timer_cb(lv_timer_t *timer) {
    (void)timer;

    if (g_result_queue == NULL) {
        return;
    }

    anim_build_result_t result = {0};
    while (xQueueReceive(g_result_queue, &result, 0) == pdPASS) {
        if (result.generation_id != g_latest_generation || result.type != g_requested_type) {
            ESP_LOGW(TAG,
                     "Discarding stale switch result gen=%lu type=%s expected_gen=%lu expected_type=%s build_ms=%lu",
                     (unsigned long)result.generation_id,
                     anim_type_name_or_none(result.type),
                     (unsigned long)g_latest_generation,
                     anim_type_name_or_none(g_requested_type),
                     (unsigned long)result.build_time_ms);
            anim_hot_discard_prepared();
            continue;
        }

        if (result.status == 0) {
            g_state = ANIM_PLAYER_SWITCH_COMMITTING;
            int64_t commit_started_us = esp_timer_get_time();
            if (activate_hot_cache(result.type, result.generation_id) == 0) {
                uint32_t commit_time_ms = (uint32_t)((esp_timer_get_time() - commit_started_us) / 1000ULL);
                log_switch_success(result.build_time_ms, commit_time_ms);
            } else {
                anim_hot_discard_prepared();
                recover_after_switch_failure(result.type, result.generation_id, result.build_time_ms, "commit_failed");
            }
        } else {
            anim_hot_discard_prepared();
            if (g_requested_type == result.type) {
                recover_after_switch_failure(result.type, result.generation_id, result.build_time_ms, "build_failed");
            }
        }
    }
}

int emoji_anim_init(lv_obj_t *img_obj) {
    if (img_obj == NULL) {
        ESP_LOGE(TAG, "Invalid image object");
        return -1;
    }

    if (anim_catalog_init() != 0) {
        ESP_LOGW(TAG, "Animation catalog init failed");
    }
    anim_warm_init_all_types();
    anim_meta_init();

    g_front_img = img_obj;
    g_current_type = EMOJI_ANIM_NONE;
    g_requested_type = EMOJI_ANIM_NONE;
    g_current_frame = 0;
    g_state = ANIM_PLAYER_IDLE;
    g_anim_start_us = esp_timer_get_time();
    g_use_cache = DEFAULT_USE_CACHE;
    clear_switch_diag();
    clear_fps_diag();

    lvgl_port_lock(0);

    lv_obj_t *parent = lv_obj_get_parent(img_obj);
    configure_anim_layer(g_front_img);
    if (g_back_img == NULL && parent != NULL) {
        g_back_img = lv_img_create(parent);
        lv_obj_set_pos(g_back_img, lv_obj_get_x(img_obj), lv_obj_get_y(img_obj));
        configure_anim_layer(g_back_img);
        lv_obj_add_flag(g_back_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(g_back_img, LV_OPA_TRANSP, 0);
    }

    if (g_frame_timer == NULL) {
        g_frame_timer = lv_timer_create(anim_frame_timer_cb, g_interval_ms, NULL);
        lv_timer_pause(g_frame_timer);
    }

    if (g_service_timer == NULL) {
        g_service_timer = lv_timer_create(anim_service_timer_cb, 20, NULL);
    }

    hide_preview_layer();
    lvgl_port_unlock();

    if (ensure_worker_started() != 0) {
        ESP_LOGW(TAG, "Animation worker unavailable, switching will fall back to sync builds");
    }
    return 0;
}

int emoji_anim_start(emoji_anim_type_t type) {
    anim_heap_snapshot_t heap_snapshot = capture_heap_snapshot();
    anim_hot_build_budget_t hot_budget = {0};
    bool low_heap_preview_only = should_use_preview_only_mode(&heap_snapshot);
    bool can_build_with_active = false;
    bool can_build_after_release = false;

    if (g_front_img == NULL) {
        ESP_LOGE(TAG, "Animation not initialized");
        return -1;
    }
    if (!anim_catalog_has_type(type)) {
        ESP_LOGW(TAG, "Animation type unavailable: %s", emoji_type_name(type));
        return -1;
    }

    if (type == g_requested_type &&
        (g_state == ANIM_PLAYER_SWITCH_PREPARING || g_state == ANIM_PLAYER_SWITCH_COMMITTING)) {
        if (ANIM_DEBUG_PERF_ENABLED) {
            ESP_LOGI(TAG,
                     "Switch request deduped while pending gen=%lu current=%s requested=%s",
                     (unsigned long)g_latest_generation,
                     anim_type_name_or_none(g_current_type),
                     anim_type_name_or_none(g_requested_type));
        }
        return 0;
    }

    if (type == g_current_type && g_state == ANIM_PLAYER_PLAYING) {
        if (ANIM_DEBUG_PERF_ENABLED) {
            ESP_LOGI(TAG, "Switch request no-op for active animation %s", anim_type_name_or_none(type));
        }
        return 0;
    }

    const lv_img_dsc_t *preview = get_preview_frame(type);
    if (preview == NULL) {
        ESP_LOGW(TAG, "No preview frame for type: %s", emoji_type_name(type));
        return -1;
    }

    ++g_latest_generation;
    clear_switch_diag();
    g_requested_type = type;
    set_playback_clock(type);
    can_build_with_active = g_use_cache && anim_hot_can_build_type(type, &hot_budget);
    can_build_after_release = g_use_cache && !low_heap_preview_only && !can_build_with_active &&
                              can_build_after_releasing_active(&hot_budget);

    if (anim_hot_is_active_type(type)) {
        log_switch_request("active-cache-hit", g_current_type, type, g_latest_generation, &heap_snapshot);
        anim_cached_frame_t *frame0 = anim_hot_get_frame(type, 0);
        if (frame0 != NULL) {
            lv_img_set_src(g_front_img, &frame0->img_dsc);
        } else {
            lv_img_set_src(g_front_img, preview);
        }
        hide_preview_layer();
        g_current_type = type;
        g_current_frame = 0;
        g_state = ANIM_PLAYER_PLAYING;
        g_anim_start_us = esp_timer_get_time();
        reset_fps_diag(type);
        resume_frame_timer_if_needed(type);
        return 0;
    }

    if (!g_use_cache || low_heap_preview_only || (!can_build_with_active && !can_build_after_release)) {
        log_switch_request("preview-only", g_current_type, type, g_latest_generation, &heap_snapshot);
        if (g_use_cache && low_heap_preview_only) {
            ESP_LOGW(TAG,
                     "Low heap headroom, using source-frame playback for %s "
                     "(internal=%u/%u bytes, dma=%u/%u bytes)",
                     emoji_type_name(type),
                     (unsigned)heap_snapshot.free_internal,
                     (unsigned)heap_snapshot.largest_internal,
                     (unsigned)heap_snapshot.free_dma,
                     (unsigned)heap_snapshot.largest_dma);
        }
        if (g_use_cache && !low_heap_preview_only && !hot_budget.can_build) {
            ESP_LOGW(TAG,
                     "Skipping async-build for %s due to PSRAM budget "
                     "(estimated=%u KB active=%u KB free=%u KB safety=%u KB hot_budget=%u KB)",
                     emoji_type_name(type),
                     (unsigned)(hot_budget.estimated_bytes / 1024U),
                     (unsigned)(hot_budget.active_bytes / 1024U),
                     (unsigned)(hot_budget.free_spiram / 1024U),
                     (unsigned)(WATCHER_ANIM_SAFETY_MARGIN_BYTES / 1024U),
                     (unsigned)(WATCHER_ANIM_HOT_BUDGET_BYTES / 1024U));
        }
        activate_preview_only(type, preview);
        log_preview_only_state("precheck", type);
        return 0;
    }

    log_switch_request("async-build", g_current_type, type, g_latest_generation, &heap_snapshot);
    begin_switch_diag(g_current_type, type, g_latest_generation, &heap_snapshot);
    if (can_build_after_release) {
        ESP_LOGW(TAG,
                 "Releasing active hot cache for %s to fit PSRAM budget "
                 "(estimated=%u KB active=%u KB free=%u KB safety=%u KB hot_budget=%u KB)",
                 emoji_type_name(type),
                 (unsigned)(hot_budget.estimated_bytes / 1024U),
                 (unsigned)(hot_budget.active_bytes / 1024U),
                 (unsigned)(hot_budget.free_spiram / 1024U),
                 (unsigned)(WATCHER_ANIM_SAFETY_MARGIN_BYTES / 1024U),
                 (unsigned)(WATCHER_ANIM_HOT_BUDGET_BYTES / 1024U));
        show_pending_preview(type, preview);
        anim_hot_release_active();
    } else {
        start_preview_fade(preview);
    }
    g_state = ANIM_PLAYER_SWITCH_PREPARING;
    if (g_frame_timer != NULL) {
        lv_timer_pause(g_frame_timer);
    }

    if (g_use_cache) {
        if (submit_build_request(type, g_latest_generation) != 0) {
            hide_preview_layer();
            recover_after_switch_failure(type, g_latest_generation, 0, "submit_failed");
            return -1;
        }
    }
    return 0;
}

void emoji_anim_stop(void) {
    if (g_frame_timer != NULL) {
        lv_timer_pause(g_frame_timer);
    }
    hide_preview_layer();
    g_state = ANIM_PLAYER_IDLE;
    g_current_type = EMOJI_ANIM_NONE;
    g_requested_type = EMOJI_ANIM_NONE;
    g_current_frame = 0;
    clear_switch_diag();
    clear_fps_diag();
}

bool emoji_anim_is_running(void) {
    return g_state == ANIM_PLAYER_PLAYING && g_current_type != EMOJI_ANIM_NONE;
}

bool emoji_anim_is_switch_pending(void) {
    return g_state == ANIM_PLAYER_SWITCH_PREPARING || g_state == ANIM_PLAYER_SWITCH_COMMITTING;
}

emoji_anim_type_t emoji_anim_get_type(void) {
    return g_current_type;
}

void emoji_anim_set_interval(uint32_t interval_ms) {
    g_interval_ms = interval_ms > 0 ? interval_ms : EMOJI_ANIM_INTERVAL_MS;
    if (g_frame_timer != NULL) {
        lv_timer_set_period(g_frame_timer, g_interval_ms);
    }
}

int emoji_anim_show_static(emoji_anim_type_t type, int frame) {
    if (g_front_img == NULL) {
        return -1;
    }

    emoji_anim_stop();

    const lv_img_dsc_t *preview = NULL;
    if (anim_hot_is_active_type(type)) {
        anim_cached_frame_t *cached = anim_hot_get_frame(type, frame);
        if (cached != NULL) {
            preview = &cached->img_dsc;
        }
    }

    if (preview == NULL) {
        preview = get_preview_frame(type);
    }

    if (preview == NULL) {
        return -1;
    }

    lv_img_set_src(g_front_img, preview);
    lv_obj_set_style_opa(g_front_img, LV_OPA_COVER, 0);
    hide_preview_layer();
    return 0;
}

int emoji_anim_prefetch_type(emoji_anim_type_t type) {
    if (!anim_catalog_has_type(type)) {
        return -1;
    }

    /* Prefetch is intentionally lightweight in the new model. */
    return 0;
}

int emoji_anim_get_fps(void) {
    return g_interval_ms > 0 ? (int)(1000U / g_interval_ms) : DEFAULT_FPS;
}

void emoji_anim_set_fps(int fps) {
    if (fps < 1) {
        fps = 1;
    } else if (fps > 60) {
        fps = 60;
    }

    emoji_anim_set_interval((uint32_t)(1000 / fps));
}
