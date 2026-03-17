#include "hal_camera.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "sensecap-watcher.h"
#include "sscma_client.h"

#define TAG "HAL_CAMERA"
#define HAL_CAMERA_CONNECT_TIMEOUT_MS CONFIG_WATCHER_CAMERA_CONNECT_TIMEOUT_MS
#define HAL_CAMERA_CAPTURE_TIMEOUT_MS CONFIG_WATCHER_CAMERA_CAPTURE_TIMEOUT_MS

typedef struct {
    sscma_client_handle_t client;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t connect_sem;
    SemaphoreHandle_t capture_sem;
    bool initialized;
    bool connected;
    bool capture_in_progress;
    esp_err_t capture_status;
    char *capture_image;
    int capture_image_size;
} hal_camera_context_t;

static hal_camera_context_t s_ctx = {
    .client = NULL,
    .lock = NULL,
    .connect_sem = NULL,
    .capture_sem = NULL,
    .initialized = false,
    .connected = false,
    .capture_in_progress = false,
    .capture_status = ESP_OK,
    .capture_image = NULL,
    .capture_image_size = 0,
};

static void hal_camera_clear_capture_locked(void) {
    if (s_ctx.capture_image != NULL) {
        free(s_ctx.capture_image);
        s_ctx.capture_image = NULL;
    }
    s_ctx.capture_image_size = 0;
    s_ctx.capture_status = ESP_OK;
}

static void hal_camera_signal_connect(void) {
    s_ctx.connected = true;
    if (s_ctx.connect_sem != NULL) {
        xSemaphoreGive(s_ctx.connect_sem);
    }
}

static bool hal_camera_extract_capture(const sscma_client_reply_t *reply) {
    char *image = NULL;
    int image_size = 0;
    bool accepted = false;

    if (reply == NULL || reply->payload == NULL) {
        return false;
    }

    if (sscma_utils_fetch_image_from_reply(reply, &image, &image_size) != ESP_OK || image == NULL || image_size <= 0) {
        return false;
    }

    if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) == pdTRUE) {
        if (s_ctx.capture_in_progress && s_ctx.capture_image == NULL) {
            s_ctx.capture_image = image;
            s_ctx.capture_image_size = image_size;
            s_ctx.capture_status = ESP_OK;
            s_ctx.capture_in_progress = false;
            accepted = true;
        }
        xSemaphoreGive(s_ctx.lock);
    }

    if (!accepted) {
        free(image);
    }

    if (accepted && s_ctx.capture_sem != NULL) {
        xSemaphoreGive(s_ctx.capture_sem);
    }

    return accepted;
}

static void hal_camera_on_connect(sscma_client_handle_t client, const sscma_client_reply_t *reply, void *user_ctx) {
    (void)client;
    (void)reply;
    (void)user_ctx;

    if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) == pdTRUE) {
        ESP_LOGI(TAG, "HX6538 connect event received");
        hal_camera_signal_connect();
        xSemaphoreGive(s_ctx.lock);
    }
}

static void hal_camera_on_response(sscma_client_handle_t client, const sscma_client_reply_t *reply, void *user_ctx) {
    (void)client;
    (void)user_ctx;
    (void)hal_camera_extract_capture(reply);
}

static void hal_camera_on_event(sscma_client_handle_t client, const sscma_client_reply_t *reply, void *user_ctx) {
    (void)client;
    (void)user_ctx;
    (void)hal_camera_extract_capture(reply);
}

static esp_err_t hal_camera_ensure_sync_primitives(void) {
    if (s_ctx.lock == NULL) {
        s_ctx.lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_ctx.lock != NULL, ESP_ERR_NO_MEM, TAG, "create lock failed");
    }

    if (s_ctx.connect_sem == NULL) {
        s_ctx.connect_sem = xSemaphoreCreateBinary();
        ESP_RETURN_ON_FALSE(s_ctx.connect_sem != NULL, ESP_ERR_NO_MEM, TAG, "create connect semaphore failed");
    }

    if (s_ctx.capture_sem == NULL) {
        s_ctx.capture_sem = xSemaphoreCreateBinary();
        ESP_RETURN_ON_FALSE(s_ctx.capture_sem != NULL, ESP_ERR_NO_MEM, TAG, "create capture semaphore failed");
    }

    return ESP_OK;
}

static void hal_camera_drain_semaphore(SemaphoreHandle_t sem) {
    if (sem == NULL) {
        return;
    }

    while (xSemaphoreTake(sem, 0) == pdTRUE) {
    }
}

static esp_err_t hal_camera_log_device_info(void) {
    sscma_client_info_t *info = NULL;
    sscma_client_model_t *model = NULL;
    sscma_client_sensor_t sensor = {0};
    esp_err_t ret;

    ret = sscma_client_get_info(s_ctx.client, &info, false);
    if (ret == ESP_OK && info != NULL) {
        ESP_LOGI(TAG, "HX6538 id=%s name=%s hw=%s fw=%s at=%s",
                 info->id ? info->id : "<null>",
                 info->name ? info->name : "<null>",
                 info->hw_ver ? info->hw_ver : "<null>",
                 info->fw_ver ? info->fw_ver : "<null>",
                 info->sw_ver ? info->sw_ver : "<null>");
    } else {
        ESP_LOGW(TAG, "sscma_client_get_info failed: %s", esp_err_to_name(ret));
    }

    ret = sscma_client_get_model(s_ctx.client, &model, false);
    if (ret == ESP_OK && model != NULL) {
        ESP_LOGI(TAG, "HX6538 model id=%d uuid=%s name=%s ver=%s",
                 model->id,
                 model->uuid ? model->uuid : "<null>",
                 model->name ? model->name : "<null>",
                 model->ver ? model->ver : "<null>");
    } else {
        ESP_LOGW(TAG, "sscma_client_get_model failed: %s", esp_err_to_name(ret));
    }

    ret = sscma_client_get_sensor(s_ctx.client, &sensor);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "HX6538 sensor id=%d type=%d state=%d opt_id=%d detail=%s",
                 sensor.id, sensor.type, sensor.state, sensor.opt_id,
                 sensor.opt_detail ? sensor.opt_detail : "<null>");
        free(sensor.opt_detail);
    } else {
        ESP_LOGW(TAG, "sscma_client_get_sensor failed: %s", esp_err_to_name(ret));
    }

    return ESP_OK;
}

static const char *hal_camera_trim_image_string(const char *image, size_t *len) {
    const char *payload = image;
    size_t payload_len;

    if (image == NULL || len == NULL) {
        return NULL;
    }

    if (strncmp(payload, "data:", 5) == 0) {
        const char *comma = strchr(payload, ',');
        if (comma != NULL) {
            payload = comma + 1;
        }
    }

    while (*payload != '\0' && isspace((unsigned char)*payload)) {
        payload++;
    }

    payload_len = strlen(payload);
    while (payload_len > 0 && isspace((unsigned char)payload[payload_len - 1])) {
        payload_len--;
    }

    *len = payload_len;
    return payload;
}

static esp_err_t hal_camera_decode_image(const char *image, uint8_t **jpeg, size_t *jpeg_size) {
    const char *payload;
    size_t payload_len = 0;
    size_t max_output = 0;
    size_t decoded_len = 0;
    uint8_t *buffer = NULL;
    int ret;

    ESP_RETURN_ON_FALSE(image != NULL && jpeg != NULL && jpeg_size != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "invalid decode arguments");

    payload = hal_camera_trim_image_string(image, &payload_len);
    ESP_RETURN_ON_FALSE(payload != NULL && payload_len > 0, ESP_ERR_INVALID_ARG, TAG, "image payload empty");

    max_output = ((payload_len + 3) / 4) * 3 + 4;
    buffer = (uint8_t *)malloc(max_output);
    ESP_RETURN_ON_FALSE(buffer != NULL, ESP_ERR_NO_MEM, TAG, "alloc jpeg buffer failed");

    ret = mbedtls_base64_decode(buffer, max_output, &decoded_len, (const unsigned char *)payload, payload_len);
    if (ret != 0) {
        free(buffer);
        ESP_LOGW(TAG, "base64 decode failed: -0x%04x", (unsigned int)(-ret));
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (decoded_len < 2 || buffer[0] != 0xFF || buffer[1] != 0xD8) {
        free(buffer);
        ESP_LOGW(TAG, "decoded payload is not a JPEG, len=%u", (unsigned int)decoded_len);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *jpeg = buffer;
    *jpeg_size = decoded_len;
    return ESP_OK;
}

esp_err_t hal_camera_init(void) {
    esp_err_t ret;
    const sscma_client_callback_t callbacks = {
        .on_connect = hal_camera_on_connect,
        .on_disconnect = NULL,
        .on_response = hal_camera_on_response,
        .on_event = hal_camera_on_event,
        .on_log = NULL,
    };

    ESP_RETURN_ON_ERROR(hal_camera_ensure_sync_primitives(), TAG, "camera sync primitive init failed");

    if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    if (s_ctx.initialized && s_ctx.connected && s_ctx.client != NULL) {
        xSemaphoreGive(s_ctx.lock);
        return ESP_OK;
    }

    s_ctx.client = bsp_sscma_client_init();
    if (s_ctx.client == NULL) {
        xSemaphoreGive(s_ctx.lock);
        ESP_LOGE(TAG, "bsp_sscma_client_init failed");
        return ESP_FAIL;
    }

    s_ctx.connected = false;
    hal_camera_clear_capture_locked();
    xSemaphoreGive(s_ctx.lock);

    hal_camera_drain_semaphore(s_ctx.connect_sem);

    ESP_RETURN_ON_ERROR(sscma_client_register_callback(s_ctx.client, &callbacks, NULL), TAG,
                        "sscma register callback failed");
    ESP_RETURN_ON_ERROR(sscma_client_init(s_ctx.client), TAG, "sscma init failed");

    ret = xSemaphoreTake(s_ctx.connect_sem, pdMS_TO_TICKS(HAL_CAMERA_CONNECT_TIMEOUT_MS)) == pdTRUE ? ESP_OK
                                                                                                     : ESP_ERR_TIMEOUT;
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HX6538 connect timeout after %d ms", HAL_CAMERA_CONNECT_TIMEOUT_MS);
        return ret;
    }

    if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) == pdTRUE) {
        s_ctx.initialized = true;
        xSemaphoreGive(s_ctx.lock);
    }

    hal_camera_log_device_info();
    return ESP_OK;
}

esp_err_t hal_camera_start(int fps, hal_camera_frame_cb_t cb, void *ctx) {
    (void)cb;
    (void)ctx;
    ESP_LOGW(TAG, "hal_camera_start(%d fps): streaming deferred", fps);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t hal_camera_stop(void) {
    ESP_LOGW(TAG, "hal_camera_stop: streaming deferred");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t hal_camera_capture_once(hal_camera_frame_cb_t cb, void *ctx) {
    esp_err_t ret;
    char *image = NULL;
    int image_size = 0;
    uint8_t *jpeg = NULL;
    size_t jpeg_size = 0;
    uint32_t timestamp_ms;

    ESP_RETURN_ON_FALSE(cb != NULL, ESP_ERR_INVALID_ARG, TAG, "frame callback required");
    ESP_RETURN_ON_ERROR(hal_camera_init(), TAG, "hal_camera_init failed");

    if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    if (s_ctx.capture_in_progress) {
        xSemaphoreGive(s_ctx.lock);
        ESP_LOGW(TAG, "capture already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    hal_camera_clear_capture_locked();
    s_ctx.capture_in_progress = true;
    s_ctx.capture_status = ESP_ERR_TIMEOUT;
    xSemaphoreGive(s_ctx.lock);

    hal_camera_drain_semaphore(s_ctx.capture_sem);

    ret = sscma_client_invoke(s_ctx.client, 1, false, true);
    if (ret != ESP_OK) {
        if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) == pdTRUE) {
            s_ctx.capture_in_progress = false;
            hal_camera_clear_capture_locked();
            xSemaphoreGive(s_ctx.lock);
        }
        ESP_LOGE(TAG, "sscma invoke failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (xSemaphoreTake(s_ctx.capture_sem, pdMS_TO_TICKS(HAL_CAMERA_CAPTURE_TIMEOUT_MS)) != pdTRUE) {
        if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) == pdTRUE) {
            s_ctx.capture_in_progress = false;
            hal_camera_clear_capture_locked();
            xSemaphoreGive(s_ctx.lock);
        }
        ESP_LOGW(TAG, "capture timed out after %d ms", HAL_CAMERA_CAPTURE_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    ret = s_ctx.capture_status;
    image = s_ctx.capture_image;
    image_size = s_ctx.capture_image_size;
    s_ctx.capture_image = NULL;
    s_ctx.capture_image_size = 0;
    xSemaphoreGive(s_ctx.lock);

    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "capture failed");
    ESP_GOTO_ON_FALSE(image != NULL && image_size > 0, ESP_ERR_INVALID_RESPONSE, cleanup, TAG, "capture image missing");
    ESP_GOTO_ON_ERROR(hal_camera_decode_image(image, &jpeg, &jpeg_size), cleanup, TAG, "image decode failed");

    timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    cb(jpeg, jpeg_size, timestamp_ms, ctx);

cleanup:
    if (image != NULL) {
        free(image);
    }
    if (jpeg != NULL) {
        free(jpeg);
    }
    return ret;
}

bool hal_camera_is_streaming(void) {
    return false;
}
