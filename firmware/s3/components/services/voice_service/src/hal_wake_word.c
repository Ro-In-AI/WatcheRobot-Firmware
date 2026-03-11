/**
 * @file hal_wake_word.c
 * @brief Wake Word Detection HAL Implementation using ESP-SR AFE
 *
 * This file implements the wake word detection HAL using ESP-SR's Audio Front-End (AFE).
 * Converted from xiaozhi-esp32's C++ AfeWakeWord class to pure C.
 *
 * Reference: xiaozhi-esp32/main/audio/wake_words/afe_wake_word.cc
 */

#include "hal_wake_word.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_heap_caps.h"
#include <string.h>

#define TAG "HAL_WAKE_WORD"

/* ------------------------------------------------------------------ */
/* Conditional Compilation                                           */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_ENABLE_WAKE_WORD

#include "esp_afe_sr_models.h"
#include "esp_nsn_models.h"
#include "model_path.h"

/* ------------------------------------------------------------------ */
/* Constants                                                         */
/* ------------------------------------------------------------------ */

#define DETECTION_RUNNING_BIT  (1 << 0)
#define MAX_WAKE_WORDS         16
#define MAX_WAKE_WORD_LEN      32
#define DETECTION_TASK_STACK   4096
#define DETECTION_TASK_PRIO    6
#define INPUT_BUFFER_CAPACITY  2048  /* samples */

/* ------------------------------------------------------------------ */
/* Context Structure                                                */
/* ------------------------------------------------------------------ */

struct wake_word_ctx_s {
    /* AFE Interface */
    const esp_afe_sr_iface_t *afe_iface;
    esp_afe_sr_data_t *afe_data;
    srmodel_list_t *models;

    /* Detection Task */
    EventGroupHandle_t event_group;
    TaskHandle_t detection_task;

    /* Callback */
    wake_word_callback_t callback;
    void *user_data;

    /* Wake word info */
    char wake_words[MAX_WAKE_WORDS][MAX_WAKE_WORD_LEN];
    int wake_word_count;
    char last_detected[MAX_WAKE_WORD_LEN];

    /* Audio configuration */
    int input_channels;
    int feed_chunk_size;

    /* Input buffer (for accumulating partial feeds) */
    int16_t *input_buffer;
    size_t input_buffer_size;
};

/* ------------------------------------------------------------------ */
/* Private: Detection Task                                             */
/* ------------------------------------------------------------------ */

static void detection_task(void *arg)
{
    wake_word_ctx_t *ctx = (wake_word_ctx_t *)arg;

    ESP_LOGI(TAG, "Detection task started");

    while (1) {
        /* Wait for detection to be enabled */
        EventBits_t bits = xEventGroupWaitBits(
            ctx->event_group,
            DETECTION_RUNNING_BIT,
            pdFALSE,      /* don't clear on exit */
            pdTRUE,       /* wait for all bits */
            portMAX_DELAY
        );

        if (!(bits & DETECTION_RUNNING_BIT)) {
            continue;
        }

        /* Wait for notification from voice_recorder_task that new data is available
         * This ensures we only fetch() after data has been fed, preventing ringbuffer empty warnings.
         * Using Task Notification (45% faster than semaphore, less RAM). */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /* Fetch detection result */
        afe_fetch_result_t *res = ctx->afe_iface->fetch(ctx->afe_data);

        if (res == NULL || res->ret_value == ESP_FAIL) {
            continue;
        }

        /* Check for wake word detection */
        if (res->wakeup_state == WAKENET_DETECTED) {
            /* Stop detection while handling callback */
            xEventGroupClearBits(ctx->event_group, DETECTION_RUNNING_BIT);

            /* Get detected wake word */
            int model_index = res->wakenet_model_index - 1;
            if (model_index >= 0 && model_index < ctx->wake_word_count) {
                strncpy(ctx->last_detected, ctx->wake_words[model_index], MAX_WAKE_WORD_LEN - 1);
                ctx->last_detected[MAX_WAKE_WORD_LEN - 1] = '\0';

                ESP_LOGI(TAG, "Wake word detected: %s", ctx->last_detected);

                /* Call user callback */
                if (ctx->callback) {
                    ctx->callback(ctx->last_detected, ctx->user_data);
                }
            } else {
                ESP_LOGW(TAG, "Wake word detected but index out of range: %d", model_index);
            }
        }
        /* Yield to allow IDLE task to run and reset watchdog */
        taskYIELD();
    }

    ESP_LOGW(TAG, "Detection task exiting (unexpected)");
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* Private: Parse wake words from model                              */
/* ------------------------------------------------------------------ */

static int parse_wake_words(wake_word_ctx_t *ctx, const char *wake_words_str)
{
    if (wake_words_str == NULL || strlen(wake_words_str) == 0) {
        return 0;
    }

    ctx->wake_word_count = 0;

    /* Parse semicolon-separated wake words */
    const char *start = wake_words_str;
    const char *end;

    while ((end = strchr(start, ';')) != NULL && ctx->wake_word_count < MAX_WAKE_WORDS) {
        size_t len = end - start;
        if (len > 0 && len < MAX_WAKE_WORD_LEN) {
            strncpy(ctx->wake_words[ctx->wake_word_count], start, len);
            ctx->wake_words[ctx->wake_word_count][len] = '\0';
            ctx->wake_word_count++;
        }
        start = end + 1;
    }

    /* Handle last wake word (no trailing semicolon) */
    if (*start != '\0' && ctx->wake_word_count < MAX_WAKE_WORDS) {
        size_t len = strlen(start);
        if (len > 0 && len < MAX_WAKE_WORD_LEN) {
            strncpy(ctx->wake_words[ctx->wake_word_count], start, len);
            ctx->wake_words[ctx->wake_word_count][len] = '\0';
            ctx->wake_word_count++;
        }
    }

    ESP_LOGI(TAG, "Parsed %d wake words", ctx->wake_word_count);
    for (int i = 0; i < ctx->wake_word_count; i++) {
        ESP_LOGI(TAG, "  [%d] %s", i, ctx->wake_words[i]);
    }

    return ctx->wake_word_count;
}

/* ------------------------------------------------------------------ */
/* Public: Initialize                                                 */
/* ------------------------------------------------------------------ */

wake_word_ctx_t *hal_wake_word_init(const wake_word_config_t *config)
{
    if (config == NULL) {
        ESP_LOGE(TAG, "Config is NULL");
        return NULL;
    }

    /* Allocate context */
    wake_word_ctx_t *ctx = (wake_word_ctx_t *)heap_caps_calloc(1, sizeof(wake_word_ctx_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ctx == NULL) {
        ESP_LOGE(TAG, "Failed to allocate context");
        return NULL;
    }

    /* Store callback */
    ctx->callback = config->callback;
    ctx->user_data = config->user_data;

    /* Create event group */
    ctx->event_group = xEventGroupCreate();
    if (ctx->event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        free(ctx);
        return NULL;
    }

    /* Initialize model list */
    if (config->model_path != NULL) {
        ctx->models = esp_srmodel_init(config->model_path);
    } else {
        ctx->models = esp_srmodel_init("model");
    }

    if (ctx->models == NULL || ctx->models->num == -1) {
        ESP_LOGE(TAG, "Failed to initialize wakenet model");
        vEventGroupDelete(ctx->event_group);
        free(ctx);
        return NULL;
    }

    /* Log available models */
    ESP_LOGI(TAG, "Total models loaded: %d", ctx->models->num);
    for (int i = 0; i < ctx->models->num; i++) {
        ESP_LOGI(TAG, "Model %d: %s", i, ctx->models->model_name[i]);
    }

    /* Find wakenet model and get wake words */
    const char *wakenet_model = NULL;
    ESP_LOGI(TAG, "Looking for wakenet model with prefix: %s", ESP_WN_PREFIX);
    for (int i = 0; i < ctx->models->num; i++) {
        ESP_LOGD(TAG, "Checking model %d: %s (contains 'wn': %s)",
                 i, ctx->models->model_name[i],
                 strstr(ctx->models->model_name[i], ESP_WN_PREFIX) ? "yes" : "no");
        if (strstr(ctx->models->model_name[i], ESP_WN_PREFIX) != NULL) {
            wakenet_model = ctx->models->model_name[i];

            /* Get wake words string */
            const char *wake_words_str = esp_srmodel_get_wake_words(ctx->models, (char *)wakenet_model);
            if (wake_words_str != NULL) {
                parse_wake_words(ctx, wake_words_str);
            }
            break;
        }
    }

    if (wakenet_model == NULL) {
        ESP_LOGE(TAG, "No wakenet model found");
        esp_srmodel_deinit(ctx->models);
        vEventGroupDelete(ctx->event_group);
        free(ctx);
        return NULL;
    }

    /* Configure AFE */
    /* Single microphone, no reference channel */
    const char *input_format = "M";  /* M = microphone, R = reference */

    afe_config_t *afe_config = afe_config_init(input_format, ctx->models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (afe_config == NULL) {
        ESP_LOGE(TAG, "Failed to init AFE config");
        esp_srmodel_deinit(ctx->models);
        vEventGroupDelete(ctx->event_group);
        free(ctx);
        return NULL;
    }

    /* Configure AFE for ESP32-S3 with PSRAM */
    afe_config->aec_init = false;             /* No acoustic echo cancellation */
    afe_config->afe_perferred_core = 1;       /* Run on core 1 */
    afe_config->afe_perferred_priority = 3;    /* Medium priority */
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;  /* Use PSRAM */

    /* Get AFE interface */
    ctx->afe_iface = esp_afe_handle_from_config(afe_config);
    if (ctx->afe_iface == NULL) {
        ESP_LOGE(TAG, "Failed to get AFE interface");
        free(afe_config);
        esp_srmodel_deinit(ctx->models);
        vEventGroupDelete(ctx->event_group);
        free(ctx);
        return NULL;
    }

    /* Create AFE instance */
    ctx->afe_data = ctx->afe_iface->create_from_config(afe_config);
    if (ctx->afe_data == NULL) {
        ESP_LOGE(TAG, "Failed to create AFE data");
        free(afe_config);
        esp_srmodel_deinit(ctx->models);
        vEventGroupDelete(ctx->event_group);
        free(ctx);
        return NULL;
    }

    /* Get feed chunk size */
    ctx->feed_chunk_size = ctx->afe_iface->get_feed_chunksize(ctx->afe_data);
    ctx->input_channels = 1;  /* Single channel */

    ESP_LOGI(TAG, "AFE initialized, feed chunk size: %d samples", ctx->feed_chunk_size);

    /* Allocate input buffer */
    ctx->input_buffer = (int16_t *)heap_caps_calloc(INPUT_BUFFER_CAPACITY, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (ctx->input_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate input buffer");
        ctx->afe_iface->destroy(ctx->afe_data);
        free(afe_config);
        esp_srmodel_deinit(ctx->models);
        vEventGroupDelete(ctx->event_group);
        free(ctx);
        return NULL;
    }
    ctx->input_buffer_size = 0;

    free(afe_config);

    /* Create detection task */
    BaseType_t ret = xTaskCreate(
        detection_task,
        "wake_detect",
        DETECTION_TASK_STACK,
        ctx,
        DETECTION_TASK_PRIO,
        &ctx->detection_task
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create detection task");
        heap_caps_free(ctx->input_buffer);
        ctx->afe_iface->destroy(ctx->afe_data);
        esp_srmodel_deinit(ctx->models);
        vEventGroupDelete(ctx->event_group);
        free(ctx);
        return NULL;
    }

    ESP_LOGI(TAG, "Wake word detector initialized successfully");
    return ctx;
}

/* ------------------------------------------------------------------ */
/* Public: Feed Audio                                                 */
/* ------------------------------------------------------------------ */

void hal_wake_word_feed(wake_word_ctx_t *ctx, const int16_t *samples, size_t num_samples)
{
    if (ctx == NULL || samples == NULL || num_samples == 0) {
        return;
    }

    /* Check if detection is running */
    if (!(xEventGroupGetBits(ctx->event_group) & DETECTION_RUNNING_BIT)) {
        return;  /* Detection is stopped */
    }

    /* Accumulate samples in input buffer */
    size_t samples_needed = num_samples;
    size_t samples_offset = 0;

    while (samples_needed > 0) {
        /* Calculate how much we can add to buffer */
        size_t space_available = INPUT_BUFFER_CAPACITY - ctx->input_buffer_size;
        size_t samples_to_add = (samples_needed < space_available) ? samples_needed : space_available;

        /* Copy samples to buffer */
        memcpy(&ctx->input_buffer[ctx->input_buffer_size], &samples[samples_offset], samples_to_add * sizeof(int16_t));
        ctx->input_buffer_size += samples_to_add;
        samples_offset += samples_to_add;
        samples_needed -= samples_to_add;

        /* Feed chunks to AFE */
        size_t chunk_size = ctx->feed_chunk_size * ctx->input_channels;
        while (ctx->input_buffer_size >= chunk_size) {
            ctx->afe_iface->feed(ctx->afe_data, ctx->input_buffer);

            /* Notify detection_task that new data has been fed
             * This ensures fetch() is called only after feed(), preventing ringbuffer empty/full issues.
             * Using Task Notification (45% faster than semaphore). */
            if (ctx->detection_task != NULL) {
                xTaskNotifyGive(ctx->detection_task);
            }

            /* Shift remaining samples to front */
            size_t remaining = ctx->input_buffer_size - chunk_size;
            if (remaining > 0) {
                memmove(ctx->input_buffer, &ctx->input_buffer[chunk_size], remaining * sizeof(int16_t));
            }
            ctx->input_buffer_size = remaining;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public: Start/Stop                                                 */
/* ------------------------------------------------------------------ */

void hal_wake_word_start(wake_word_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    xEventGroupSetBits(ctx->event_group, DETECTION_RUNNING_BIT);

    /* Note: Task Notification sync in detection_task ensures fetch() is only called after feed().
     * No artificial delay needed - the synchronization happens automatically. */

    ESP_LOGI(TAG, "Wake word detection started");
}

void hal_wake_word_stop(wake_word_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    xEventGroupClearBits(ctx->event_group, DETECTION_RUNNING_BIT);

    /* Clear input buffer */
    if (ctx->input_buffer != NULL) {
        ctx->input_buffer_size = 0;
    }

    /* Reset AFE buffer */
    if (ctx->afe_data != NULL && ctx->afe_iface != NULL) {
        ctx->afe_iface->reset_buffer(ctx->afe_data);
    }

    ESP_LOGI(TAG, "Wake word detection stopped");
}

/* ------------------------------------------------------------------ */
/* Public: Get Feed Size                                              */
/* ------------------------------------------------------------------ */

size_t hal_wake_word_get_feed_size(wake_word_ctx_t *ctx)
{
    if (ctx == NULL || ctx->afe_data == NULL) {
        return 0;
    }
    return ctx->feed_chunk_size;
}

/* ------------------------------------------------------------------ */
/* Public: Deinitialize                                               */
/* ------------------------------------------------------------------ */

void hal_wake_word_deinit(wake_word_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    /* Stop detection first */
    hal_wake_word_stop(ctx);

    /* Delete detection task */
    if (ctx->detection_task != NULL) {
        vTaskDelete(ctx->detection_task);
        ctx->detection_task = NULL;
    }

    /* Free input buffer */
    if (ctx->input_buffer != NULL) {
        heap_caps_free(ctx->input_buffer);
        ctx->input_buffer = NULL;
    }

    /* Destroy AFE */
    if (ctx->afe_data != NULL && ctx->afe_iface != NULL) {
        ctx->afe_iface->destroy(ctx->afe_data);
        ctx->afe_data = NULL;
    }

    /* Deinit models */
    if (ctx->models != NULL) {
        esp_srmodel_deinit(ctx->models);
        ctx->models = NULL;
    }

    /* Delete event group */
    if (ctx->event_group != NULL) {
        vEventGroupDelete(ctx->event_group);
        ctx->event_group = NULL;
    }

    /* Free context */
    free(ctx);

    ESP_LOGI(TAG, "Wake word detector deinitialized");
}

/* ------------------------------------------------------------------ */
/* Public: Is Supported                                               */
/* ------------------------------------------------------------------ */

bool hal_wake_word_is_supported(void)
{
    /* ESP-SR requires ESP32-S3 with PSRAM */
#ifdef CONFIG_IDF_TARGET_ESP32S3
#ifdef CONFIG_SPIRAM
    return true;
#endif
#endif
    return false;
}

/* ------------------------------------------------------------------ */
/* Public: Get Last Detected                                          */
/* ------------------------------------------------------------------ */

const char *hal_wake_word_get_last_detected(wake_word_ctx_t *ctx)
{
    if (ctx == NULL) {
        return NULL;
    }
    return ctx->last_detected[0] != '\0' ? ctx->last_detected : NULL;
}

/* ------------------------------------------------------------------ */
/* Public: Get Available Wake Words                                   */
/* ------------------------------------------------------------------ */

int hal_wake_word_get_available_list(wake_word_ctx_t *ctx, char *out_buf, size_t buf_size)
{
    if (ctx == NULL || out_buf == NULL || buf_size == 0) {
        return 0;
    }

    size_t pos = 0;
    for (int i = 0; i < ctx->wake_word_count && pos < buf_size - 1; i++) {
        if (i > 0) {
            out_buf[pos++] = ';';
        }
        size_t len = strlen(ctx->wake_words[i]);
        if (pos + len >= buf_size) {
            break;
        }
        memcpy(&out_buf[pos], ctx->wake_words[i], len);
        pos += len;
    }
    out_buf[pos] = '\0';

    return (int)pos;
}

#else /* !CONFIG_ENABLE_WAKE_WORD */

/* ------------------------------------------------------------------ */
/* Stub Implementation (Wake Word Disabled)                           */
/* ------------------------------------------------------------------ */

wake_word_ctx_t *hal_wake_word_init(const wake_word_config_t *config)
{
    (void)config;
    ESP_LOGW(TAG, "Wake word detection is disabled (CONFIG_ENABLE_WAKE_WORD=n)");
    return NULL;
}

void hal_wake_word_feed(wake_word_ctx_t *ctx, const int16_t *samples, size_t num_samples)
{
    (void)ctx;
    (void)samples;
    (void)num_samples;
}

void hal_wake_word_start(wake_word_ctx_t *ctx)
{
    (void)ctx;
}

void hal_wake_word_stop(wake_word_ctx_t *ctx)
{
    (void)ctx;
}

size_t hal_wake_word_get_feed_size(wake_word_ctx_t *ctx)
{
    (void)ctx;
    return 0;
}

void hal_wake_word_deinit(wake_word_ctx_t *ctx)
{
    (void)ctx;
}

bool hal_wake_word_is_supported(void)
{
    return false;
}

const char *hal_wake_word_get_last_detected(wake_word_ctx_t *ctx)
{
    (void)ctx;
    return NULL;
}

int hal_wake_word_get_available_list(wake_word_ctx_t *ctx, char *out_buf, size_t buf_size)
{
    (void)ctx;
    (void)out_buf;
    (void)buf_size;
    return 0;
}

#endif /* CONFIG_ENABLE_WAKE_WORD */
