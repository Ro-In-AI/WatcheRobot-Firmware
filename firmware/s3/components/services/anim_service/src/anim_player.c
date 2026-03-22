/**
 * @file anim_player.c
 * @brief Animation player with warm-cache previews and async hot-cache switching.
 */

#include "anim_player.h"
#include "anim_meta.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define TAG "ANIM_PLAYER"

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

#define ANIM_SOURCE_FRAME_SIZE 206
#define ANIM_SOURCE_FRAME_PIVOT (ANIM_SOURCE_FRAME_SIZE / 2)
#define ANIM_DISPLAY_ZOOM_2X (LV_IMG_ZOOM_NONE * 2)

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

static void anim_worker_task(void *param);
static void anim_frame_timer_cb(lv_timer_t *timer);
static void anim_service_timer_cb(lv_timer_t *timer);

static void set_obj_opa(void *obj, int32_t value) {
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void configure_anim_layer(lv_obj_t *img_obj) {
    if (img_obj == NULL) {
        return;
    }

    lv_img_set_pivot(img_obj, ANIM_SOURCE_FRAME_PIVOT, ANIM_SOURCE_FRAME_PIVOT);
    lv_img_set_zoom(img_obj, ANIM_DISPLAY_ZOOM_2X);
    lv_img_set_antialias(img_obj, false);
}

static void hide_preview_layer(void) {
    if (g_back_img == NULL) {
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
    int fps = anim_meta_get_fps(type);
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
    int frame_count = anim_hot_get_frame_count();
    if (g_frame_timer == NULL) {
        return;
    }
    if (anim_hot_is_active_type(type) && frame_count > 1) {
        lv_timer_resume(g_frame_timer);
    } else {
        lv_timer_pause(g_frame_timer);
    }
}

static void activate_hot_cache(emoji_anim_type_t type, uint32_t generation_id) {
    if (anim_hot_commit_prepared(type, generation_id) != 0) {
        return;
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
    resume_frame_timer_if_needed(type);
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

static void anim_worker_task(void *param) {
    (void)param;

    for (;;) {
        anim_build_request_t request = {0};
        if (xQueueReceive(g_request_queue, &request, portMAX_DELAY) != pdPASS) {
            continue;
        }

        int status = -1;
        if (request.generation_id == g_latest_generation && request.type == g_requested_type) {
            status = anim_hot_build_type(request.type, request.generation_id);
        }

        anim_build_result_t result = {
            .type = request.type,
            .generation_id = request.generation_id,
            .status = status,
        };
        xQueueOverwrite(g_result_queue, &result);
    }
}

static void anim_frame_timer_cb(lv_timer_t *timer) {
    (void)timer;

    if (g_state != ANIM_PLAYER_PLAYING || g_current_type == EMOJI_ANIM_NONE || !anim_hot_is_active_type(g_current_type)) {
        return;
    }

    int frame_count = anim_hot_get_frame_count();
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

    anim_cached_frame_t *frame = anim_hot_get_frame(g_current_type, frame_index);
    if (frame != NULL && g_front_img != NULL) {
        lv_img_set_src(g_front_img, &frame->img_dsc);
        g_current_frame = frame_index;
    }
}

static void anim_service_timer_cb(lv_timer_t *timer) {
    (void)timer;

    if (g_result_queue == NULL) {
        return;
    }

    anim_build_result_t result = {0};
    while (xQueueReceive(g_result_queue, &result, 0) == pdPASS) {
        if (result.generation_id != g_latest_generation || result.type != g_requested_type) {
            anim_hot_discard_prepared();
            continue;
        }

        if (result.status == 0) {
            g_state = ANIM_PLAYER_SWITCH_COMMITTING;
            activate_hot_cache(result.type, result.generation_id);
        } else {
            anim_hot_discard_prepared();
            if (g_requested_type == result.type) {
                g_state = ANIM_PLAYER_IDLE;
                lv_timer_pause(g_frame_timer);
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
    configure_anim_layer(g_front_img);
    g_current_type = EMOJI_ANIM_NONE;
    g_requested_type = EMOJI_ANIM_NONE;
    g_current_frame = 0;
    g_state = ANIM_PLAYER_IDLE;
    g_anim_start_us = esp_timer_get_time();
    g_use_cache = DEFAULT_USE_CACHE;

    lv_obj_t *parent = lv_obj_get_parent(img_obj);
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

    if (ensure_worker_started() != 0) {
        ESP_LOGW(TAG, "Animation worker unavailable, switching will fall back to sync builds");
    }

    hide_preview_layer();
    return 0;
}

int emoji_anim_start(emoji_anim_type_t type) {
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
        return 0;
    }

    if (type == g_current_type && g_state == ANIM_PLAYER_PLAYING) {
        return 0;
    }

    const lv_img_dsc_t *preview = get_preview_frame(type);
    if (preview == NULL) {
        ESP_LOGW(TAG, "No preview frame for type: %s", emoji_type_name(type));
        return -1;
    }

    ++g_latest_generation;
    g_requested_type = type;
    set_playback_clock(type);

    if (anim_hot_is_active_type(type)) {
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
        resume_frame_timer_if_needed(type);
        return 0;
    }

    start_preview_fade(preview);
    g_state = ANIM_PLAYER_SWITCH_PREPARING;
    if (g_frame_timer != NULL) {
        lv_timer_pause(g_frame_timer);
    }

    if (g_use_cache) {
        if (submit_build_request(type, g_latest_generation) != 0) {
            hide_preview_layer();
            g_state = ANIM_PLAYER_IDLE;
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
}

bool emoji_anim_is_running(void) {
    return g_state == ANIM_PLAYER_PLAYING && g_current_type != EMOJI_ANIM_NONE;
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
