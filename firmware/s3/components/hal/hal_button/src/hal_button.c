/**
 * HAL Button Driver for MVP-W
 *
 * Reuses SDK's IO Expander handle to avoid I2C re-initialization conflict
 */
#include "hal_button.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensecap-watcher.h"

#define TAG "HAL_BUTTON"
#define BUTTON_IO_EXPANDER_PROBE_COOLDOWN_MS 5000
#define BUTTON_IO_EXPANDER_READ_RETRY_COOLDOWN_MS 1000

/* Use SDK's button pin definition (already a mask: 1ULL << 3) */
#define BUTTON_PIN_MASK BSP_KNOB_BTN

/* Debounce time in ms */
#define DEBOUNCE_MS 50

static button_callback_t g_callback = NULL;
static bool g_is_pressed = false;
static int64_t g_last_change_time = 0;
static bool g_backend_ready = false;
static bool g_probe_cached = false;
static bool g_probe_ready = false;
static int64_t g_last_probe_time_ms = 0;
static int64_t g_last_read_error_log_ms = 0;

static esp_err_t hal_button_read_level(uint8_t *out_level) {
    if (out_level == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_io_expander_handle_t io_exp = bsp_io_expander_init();
    if (io_exp == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t level_mask = 0;
    esp_err_t ret = esp_io_expander_get_level(io_exp, BUTTON_PIN_MASK, &level_mask);
    if (ret != ESP_OK) {
        return ret;
    }

    *out_level = (level_mask & BUTTON_PIN_MASK) ? 1U : 0U;
    return ESP_OK;
}

bool hal_button_io_ready(void) {
    int64_t now_ms = esp_timer_get_time() / 1000;
    uint8_t level = 0;

    if (g_probe_cached) {
        if (g_probe_ready) {
            ESP_LOGI(TAG, "IO expander button probe cached as ready");
            return true;
        }
        if (now_ms - g_last_probe_time_ms < BUTTON_IO_EXPANDER_PROBE_COOLDOWN_MS) {
            ESP_LOGW(TAG,
                     "IO expander button probe still cooling down (%lld ms remaining)",
                     (long long)(BUTTON_IO_EXPANDER_PROBE_COOLDOWN_MS - (now_ms - g_last_probe_time_ms)));
            return false;
        }
    }

    esp_err_t ret = hal_button_read_level(&level);
    g_last_probe_time_ms = now_ms;
    g_probe_cached = true;
    g_probe_ready = (ret == ESP_OK);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "IO expander button probe failed: %s", esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(TAG, "IO expander button probe succeeded (level=%u)", (unsigned)level);
    return true;
}

/* Poll button state (called from task context) */
void hal_button_poll(void) {
    if (!g_backend_ready) {
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    if (g_probe_cached && !g_probe_ready && now_ms - g_last_probe_time_ms < BUTTON_IO_EXPANDER_READ_RETRY_COOLDOWN_MS) {
        return;
    }

    /* Read the button bit directly from the expander input register. */
    uint8_t level = 0;
    esp_err_t ret = hal_button_read_level(&level);
    if (ret != ESP_OK) {
        if (now_ms - g_last_read_error_log_ms >= BUTTON_IO_EXPANDER_READ_RETRY_COOLDOWN_MS) {
            ESP_LOGW(TAG, "Button read failed, keeping previous state: %s", esp_err_to_name(ret));
            g_last_read_error_log_ms = now_ms;
        }
        g_probe_cached = true;
        g_probe_ready = false;
        g_last_probe_time_ms = now_ms;
        return;
    }
    g_probe_cached = true;
    g_probe_ready = true;
    g_last_probe_time_ms = now_ms;

    /* Button is active low (0 = pressed) */
    bool current_pressed = (level == 0);

    /* Check for state change with debounce */
    if (current_pressed != g_is_pressed) {
        if (now_ms - g_last_change_time >= DEBOUNCE_MS) {
            g_is_pressed = current_pressed;
            g_last_change_time = now_ms;

            ESP_LOGI(TAG, "Button %s", g_is_pressed ? "PRESSED" : "RELEASED");

            /* Call callback (safe for task context) */
            if (g_callback) {
                g_callback(g_is_pressed);
            }
        }
    }
}

int hal_button_init(button_callback_t callback) {
    ESP_LOGI(TAG, "Initializing button via IO Expander...");

    g_callback = callback;
    g_is_pressed = false;
    g_last_change_time = 0;
    g_backend_ready = false;

    /* Use SDK's already initialized IO expander */
    esp_io_expander_handle_t io_exp = bsp_io_expander_init();
    if (io_exp == NULL) {
        ESP_LOGE(TAG, "Failed to get IO expander handle");
        return -1;
    }
    if (!hal_button_io_ready()) {
        ESP_LOGW(TAG, "Button backend unavailable, skipping voice button input");
        return -1;
    }

    /* Set button pin as input */
    esp_err_t ret = esp_io_expander_set_dir(io_exp, BUTTON_PIN_MASK, IO_EXPANDER_INPUT);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Set dir failed: %s", esp_err_to_name(ret));
    }

    /* Read initial state using SDK function */
    uint8_t level = 0;
    ret = hal_button_read_level(&level);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Initial button read failed: %s", esp_err_to_name(ret));
        return -1;
    }
    g_is_pressed = (level == 0);
    g_backend_ready = true;
    g_probe_cached = true;
    g_probe_ready = true;

    ESP_LOGI(TAG, "Button initialized, initial state: %s", g_is_pressed ? "pressed" : "released");

    return 0;
}

bool hal_button_is_pressed(void) {
    return g_is_pressed;
}

void hal_button_deinit(void) {
    /* Don't delete IO expander handle - it's managed by SDK */
    g_callback = NULL;
    g_backend_ready = false;
}
