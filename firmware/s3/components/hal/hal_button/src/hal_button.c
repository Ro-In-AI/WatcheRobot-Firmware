/**
 * HAL Button Driver for MVP-W
 *
 * Reuses SDK's IO Expander handle to avoid I2C re-initialization conflict
 */
#include "hal_button.h"
#include "sensecap-watcher.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "HAL_BUTTON"

/* Use SDK's button pin definition (already a mask: 1ULL << 3) */
#define BUTTON_PIN_MASK         BSP_KNOB_BTN

/* Debounce time in ms */
#define DEBOUNCE_MS             50

static button_callback_t g_callback = NULL;
static bool g_is_pressed = false;
static int64_t g_last_change_time = 0;

/* Poll button state (called from task context) */
void hal_button_poll(void)
{
    /* Use SDK's IO expander level reader */
    uint8_t level = bsp_exp_io_get_level(BUTTON_PIN_MASK);

    /* Button is active low (0 = pressed) */
    bool current_pressed = (level == 0);

    int64_t now = esp_timer_get_time() / 1000;  /* ms */

    /* Check for state change with debounce */
    if (current_pressed != g_is_pressed) {
        if (now - g_last_change_time >= DEBOUNCE_MS) {
            g_is_pressed = current_pressed;
            g_last_change_time = now;

            ESP_LOGI(TAG, "Button %s", g_is_pressed ? "PRESSED" : "RELEASED");

            /* Call callback (safe for task context) */
            if (g_callback) {
                g_callback(g_is_pressed);
            }
        }
    }
}

int hal_button_init(button_callback_t callback)
{
    ESP_LOGI(TAG, "Initializing button via IO Expander...");

    g_callback = callback;
    g_is_pressed = false;
    g_last_change_time = 0;

    /* Use SDK's already initialized IO expander */
    esp_io_expander_handle_t io_exp = bsp_io_expander_init();
    if (io_exp == NULL) {
        ESP_LOGE(TAG, "Failed to get IO expander handle");
        return -1;
    }

    /* Set button pin as input */
    esp_err_t ret = esp_io_expander_set_dir(io_exp, BUTTON_PIN_MASK, IO_EXPANDER_INPUT);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Set dir failed: %s", esp_err_to_name(ret));
    }

    /* Read initial state using SDK function */
    uint8_t level = bsp_exp_io_get_level(BUTTON_PIN_MASK);
    g_is_pressed = (level == 0);

    ESP_LOGI(TAG, "Button initialized, initial state: %s",
             g_is_pressed ? "pressed" : "released");

    return 0;
}

bool hal_button_is_pressed(void)
{
    return g_is_pressed;
}

void hal_button_deinit(void)
{
    /* Don't delete IO expander handle - it's managed by SDK */
    g_callback = NULL;
}
