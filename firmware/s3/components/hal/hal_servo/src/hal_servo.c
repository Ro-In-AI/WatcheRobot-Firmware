/**
 * @file hal_servo.c
 * @brief Servo HAL: LEDC PWM direct drive on GPIO 19 (X) and GPIO 20 (Y)
 *
 * Phase 2 implementation: LEDC timer + smooth-move FreeRTOS task.
 *
 * GPIO mapping (from CLAUDE.md):
 *   GPIO 19 → Servo X axis (LEDC channel 0)
 *   GPIO 20 → Servo Y axis (LEDC channel 1)
 *
 * PWM Configuration:
 *   - 50Hz frequency (20ms period)
 *   - 14-bit resolution (16384 levels)
 *   - Pulse width: 1ms (0 deg) to 2ms (180 deg)
 */

#include "hal_servo.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <ctype.h>
#include <string.h>

#define TAG "HAL_SERVO"

/* LEDC Configuration */
#define LEDC_TIMER_NUM LEDC_TIMER_0
#define LEDC_SPEED_MODE LEDC_LOW_SPEED_MODE
#define LEDC_DUTY_RES LEDC_TIMER_14_BIT
#define LEDC_FREQ_HZ 50
#define LEDC_CHANNEL_X LEDC_CHANNEL_0
#define LEDC_CHANNEL_Y LEDC_CHANNEL_1

/* Servo PWM timing (typical hobby servo) */
#define SERVO_PERIOD_US 20000   /* 20ms for 50Hz */
#define SERVO_MIN_PULSE_US 1000 /* 1ms = 0 degrees */
#define SERVO_MAX_PULSE_US 2000 /* 2ms = 180 degrees */
#define SERVO_RANGE_DEG 180

/* Duty cycle values for 14-bit resolution */
#define DUTY_RESOLUTION 16384
#define DUTY_MIN (SERVO_MIN_PULSE_US * DUTY_RESOLUTION / SERVO_PERIOD_US) /* ~819 */
#define DUTY_MAX (SERVO_MAX_PULSE_US * DUTY_RESOLUTION / SERVO_PERIOD_US) /* ~1638 */

/* Smooth move task configuration */
#define SERVO_TASK_STACK_SIZE 2048
#define SERVO_TASK_PRIORITY 5
#define SERVO_CMD_QUEUE_SIZE 100

/** Synchronized dual-axis move command */
typedef struct {
    int x_deg;
    int y_deg;
    int duration_ms;
} servo_sync_cmd_t;

/** Single-axis move command */
typedef struct {
    servo_axis_t axis;
    int angle_deg;
    int duration_ms;
} servo_move_cmd_t;

/** Combined command type for queue */
typedef enum { CMD_TYPE_SINGLE, CMD_TYPE_SYNC } cmd_type_t;

typedef struct {
    cmd_type_t type;
    union {
        servo_move_cmd_t single;
        servo_sync_cmd_t sync;
    };
} servo_cmd_msg_t;

/* State variables */
static int s_angle[2] = {90, 90}; /* X default center, Y default 120° */
static bool s_initialized = false;
static QueueHandle_t s_cmd_queue = NULL;
static TaskHandle_t s_servo_task = NULL;
static SemaphoreHandle_t s_angle_mutex = NULL;

/* Forward declarations */
static void servo_task(void *arg);
static int angle_to_duty(int angle_deg);
static int servo_map_physical_angle(servo_axis_t axis, int logical_angle_deg);
static int angle_to_duty_mapped(servo_axis_t axis, int logical_angle_deg);
static esp_err_t set_duty(servo_axis_t axis, int duty);
static esp_err_t configure_ledc(void);
static void move_to_angle_immediate(servo_axis_t axis, int angle_deg);

/**
 * @brief Convert angle in degrees to LEDC duty cycle value.
 *
 * @param angle_deg Angle 0-180 degrees
 * @return Duty cycle value for 14-bit resolution
 */
static int angle_to_duty(int angle_deg) {
    if (angle_deg < 0)
        angle_deg = 0;
    if (angle_deg > SERVO_RANGE_DEG)
        angle_deg = SERVO_RANGE_DEG;

    /* Linear interpolation: duty = DUTY_MIN + (angle * (DUTY_MAX - DUTY_MIN) / 180) */
    return DUTY_MIN + (angle_deg * (DUTY_MAX - DUTY_MIN) / SERVO_RANGE_DEG);
}

/**
 * @brief Map logical control angle to physical servo angle.
 *
 * Current hardware wiring expects direct mapping:
 * logical angle == physical angle for both X and Y axis.
 */
static int servo_map_physical_angle(servo_axis_t axis, int logical_angle_deg) {
    (void)axis;
    int physical = logical_angle_deg;

    if (physical < 0)
        physical = 0;
    if (physical > SERVO_RANGE_DEG)
        physical = SERVO_RANGE_DEG;
    return physical;
}

static int angle_to_duty_mapped(servo_axis_t axis, int logical_angle_deg) {
    return angle_to_duty(servo_map_physical_angle(axis, logical_angle_deg));
}

/**
 * @brief Set LEDC duty cycle for a servo channel.
 *
 * @param axis Servo axis (X or Y)
 * @param duty Duty cycle value
 * @return ESP_OK on success
 */
static esp_err_t set_duty(servo_axis_t axis, int duty) {
    ledc_channel_t channel = (axis == SERVO_AXIS_X) ? LEDC_CHANNEL_X : LEDC_CHANNEL_Y;

    esp_err_t ret = ledc_set_duty(LEDC_SPEED_MODE, channel, duty);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set duty for axis %d: %s", axis, esp_err_to_name(ret));
        return ret;
    }

    ret = ledc_update_duty(LEDC_SPEED_MODE, channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update duty for axis %d: %s", axis, esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

/**
 * @brief Configure LEDC timer and channels.
 *
 * @return ESP_OK on success, ESP_FAIL on configuration error
 */
static esp_err_t configure_ledc(void) {
    /* Timer configuration */
    ledc_timer_config_t timer_cfg = {.speed_mode = LEDC_SPEED_MODE,
                                     .duty_resolution = LEDC_DUTY_RES,
                                     .timer_num = LEDC_TIMER_NUM,
                                     .freq_hz = LEDC_FREQ_HZ,
                                     .clk_cfg = LEDC_AUTO_CLK};

    esp_err_t ret = ledc_timer_config(&timer_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer config failed: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    /* X-axis channel (GPIO 19) */
    ledc_channel_config_t ch_x = {.gpio_num = CONFIG_WATCHER_SERVO_X_GPIO,
                                  .speed_mode = LEDC_SPEED_MODE,
                                  .channel = LEDC_CHANNEL_X,
                                  .timer_sel = LEDC_TIMER_NUM,
                                  .duty = angle_to_duty_mapped(SERVO_AXIS_X, s_angle[SERVO_AXIS_X]),
                                  .hpoint = 0};

    ret = ledc_channel_config(&ch_x);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LEDC channel X config failed: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    /* Y-axis channel (GPIO 20) */
    ledc_channel_config_t ch_y = {.gpio_num = CONFIG_WATCHER_SERVO_Y_GPIO,
                                  .speed_mode = LEDC_SPEED_MODE,
                                  .channel = LEDC_CHANNEL_Y,
                                  .timer_sel = LEDC_TIMER_NUM,
                                  .duty = angle_to_duty_mapped(SERVO_AXIS_Y, s_angle[SERVO_AXIS_Y]),
                                  .hpoint = 0};

    ret = ledc_channel_config(&ch_y);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LEDC channel Y config failed: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "LEDC configured: %dHz, %d-bit, GPIO %d (X), GPIO %d (Y)", LEDC_FREQ_HZ, LEDC_DUTY_RES,
             CONFIG_WATCHER_SERVO_X_GPIO, CONFIG_WATCHER_SERVO_Y_GPIO);

    return ESP_OK;
}

/**
 * @brief Move servo to angle immediately (no smoothing).
 *
 * @param axis Servo axis
 * @param angle_deg Target angle
 */
static void move_to_angle_immediate(servo_axis_t axis, int angle_deg) {
    int duty = angle_to_duty_mapped(axis, angle_deg);
    set_duty(axis, duty);

    if (xSemaphoreTake(s_angle_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_angle[axis] = angle_deg;
        xSemaphoreGive(s_angle_mutex);
    }
}

/**
 * @brief Background task for smooth servo movement.
 *
 * Processes commands from queue and performs linear interpolation.
 *
 * @param arg Unused
 */
static void servo_task(void *arg) {
    (void)arg;
    servo_cmd_msg_t cmd;
    const TickType_t step_interval = pdMS_TO_TICKS(CONFIG_WATCHER_SERVO_SMOOTH_STEP_MS);

    ESP_LOGI(TAG, "Servo task started (step interval: %dms)", CONFIG_WATCHER_SERVO_SMOOTH_STEP_MS);

    while (1) {
        if (xQueueReceive(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
            /* No command, continue waiting */
            continue;
        }

        if (cmd.type == CMD_TYPE_SINGLE) {
            /* Single-axis smooth move */
            servo_axis_t axis = cmd.single.axis;
            int target_deg = cmd.single.angle_deg;
            int duration_ms = cmd.single.duration_ms;

            /* Clamp Y-axis to mechanical limits */
            if (axis == SERVO_AXIS_Y) {
                if (target_deg < CONFIG_WATCHER_SERVO_Y_MIN_DEG) {
                    target_deg = CONFIG_WATCHER_SERVO_Y_MIN_DEG;
                }
                if (target_deg > CONFIG_WATCHER_SERVO_Y_MAX_DEG) {
                    target_deg = CONFIG_WATCHER_SERVO_Y_MAX_DEG;
                }
            }

            /* Get current angle */
            int start_deg;
            if (xSemaphoreTake(s_angle_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                start_deg = s_angle[axis];
                xSemaphoreGive(s_angle_mutex);
            } else {
                continue;
            }

            /* Skip if already at target */
            if (start_deg == target_deg) {
                continue;
            }

            /* Calculate steps */
            int num_steps = duration_ms / CONFIG_WATCHER_SERVO_SMOOTH_STEP_MS;
            if (num_steps < 1)
                num_steps = 1;

            float step_size = (float)(target_deg - start_deg) / num_steps;

            ESP_LOGD(TAG, "Smooth move: axis=%s, start=%d, target=%d, steps=%d", axis == SERVO_AXIS_X ? "X" : "Y",
                     start_deg, target_deg, num_steps);

            /* Interpolate */
            for (int i = 1; i <= num_steps; i++) {
                int current_deg;
                if (i == num_steps) {
                    current_deg = target_deg;
                } else {
                    current_deg = start_deg + (int)(step_size * i);
                }

                int duty = angle_to_duty_mapped(axis, current_deg);
                set_duty(axis, duty);

                /* Update stored angle */
                if (xSemaphoreTake(s_angle_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                    s_angle[axis] = current_deg;
                    xSemaphoreGive(s_angle_mutex);
                }

                vTaskDelay(step_interval);
            }

        } else if (cmd.type == CMD_TYPE_SYNC) {
            /* Synchronized dual-axis move */
            int target_x = cmd.sync.x_deg;
            int target_y = cmd.sync.y_deg;
            int duration_ms = cmd.sync.duration_ms;

            /* Clamp Y-axis to mechanical limits */
            if (target_y < CONFIG_WATCHER_SERVO_Y_MIN_DEG) {
                target_y = CONFIG_WATCHER_SERVO_Y_MIN_DEG;
            }
            if (target_y > CONFIG_WATCHER_SERVO_Y_MAX_DEG) {
                target_y = CONFIG_WATCHER_SERVO_Y_MAX_DEG;
            }

            /* Get current angles */
            int start_x, start_y;
            if (xSemaphoreTake(s_angle_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                start_x = s_angle[SERVO_AXIS_X];
                start_y = s_angle[SERVO_AXIS_Y];
                xSemaphoreGive(s_angle_mutex);
            } else {
                continue;
            }

            /* Calculate steps */
            int num_steps = duration_ms / CONFIG_WATCHER_SERVO_SMOOTH_STEP_MS;
            if (num_steps < 1)
                num_steps = 1;

            float step_x = (float)(target_x - start_x) / num_steps;
            float step_y = (float)(target_y - start_y) / num_steps;

            ESP_LOGD(TAG, "Sync move: X %d->%d, Y %d->%d, steps=%d", start_x, target_x, start_y, target_y, num_steps);

            /* Interpolate both axes */
            for (int i = 1; i <= num_steps; i++) {
                int current_x, current_y;

                if (i == num_steps) {
                    current_x = target_x;
                    current_y = target_y;
                } else {
                    current_x = start_x + (int)(step_x * i);
                    current_y = start_y + (int)(step_y * i);
                }

                int duty_x = angle_to_duty_mapped(SERVO_AXIS_X, current_x);
                int duty_y = angle_to_duty_mapped(SERVO_AXIS_Y, current_y);

                set_duty(SERVO_AXIS_X, duty_x);
                set_duty(SERVO_AXIS_Y, duty_y);

                /* Update stored angles */
                if (xSemaphoreTake(s_angle_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                    s_angle[SERVO_AXIS_X] = current_x;
                    s_angle[SERVO_AXIS_Y] = current_y;
                    xSemaphoreGive(s_angle_mutex);
                }

                vTaskDelay(step_interval);
            }
        }
    }
}

esp_err_t hal_servo_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }

    /* Create mutex for thread-safe angle access */
    s_angle_mutex = xSemaphoreCreateMutex();
    if (s_angle_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create angle mutex");
        return ESP_FAIL;
    }

    /* Configure LEDC */
    esp_err_t ret = configure_ledc();
    if (ret != ESP_OK) {
        vSemaphoreDelete(s_angle_mutex);
        s_angle_mutex = NULL;
        return ret;
    }

    /* Create command queue */
    s_cmd_queue = xQueueCreate(SERVO_CMD_QUEUE_SIZE, sizeof(servo_cmd_msg_t));
    if (s_cmd_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create command queue");
        vSemaphoreDelete(s_angle_mutex);
        s_angle_mutex = NULL;
        return ESP_FAIL;
    }

    /* Create servo task */
    BaseType_t task_ret =
        xTaskCreate(servo_task, "servo_task", SERVO_TASK_STACK_SIZE, NULL, SERVO_TASK_PRIORITY, &s_servo_task);

    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create servo task");
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        vSemaphoreDelete(s_angle_mutex);
        s_angle_mutex = NULL;
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Servo HAL initialized: X=GPIO%d, Y=GPIO%d, Y limits=[%d,%d]deg", CONFIG_WATCHER_SERVO_X_GPIO,
             CONFIG_WATCHER_SERVO_Y_GPIO, CONFIG_WATCHER_SERVO_Y_MIN_DEG, CONFIG_WATCHER_SERVO_Y_MAX_DEG);

    return ESP_OK;
}

esp_err_t hal_servo_set_angle(servo_axis_t axis, int angle_deg) {
    if (!s_initialized) {
        ESP_LOGW(TAG, "Servo not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (axis != SERVO_AXIS_X && axis != SERVO_AXIS_Y) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Validate angle range */
    if (angle_deg < 0 || angle_deg > 180) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Clamp Y-axis to mechanical limits */
    if (axis == SERVO_AXIS_Y) {
        if (angle_deg < CONFIG_WATCHER_SERVO_Y_MIN_DEG) {
            angle_deg = CONFIG_WATCHER_SERVO_Y_MIN_DEG;
        }
        if (angle_deg > CONFIG_WATCHER_SERVO_Y_MAX_DEG) {
            angle_deg = CONFIG_WATCHER_SERVO_Y_MAX_DEG;
        }
    }

    move_to_angle_immediate(axis, angle_deg);
    ESP_LOGI(TAG, "Set angle: axis=%s, angle=%d", axis == SERVO_AXIS_X ? "X" : "Y", angle_deg);

    return ESP_OK;
}

esp_err_t hal_servo_move_smooth(servo_axis_t axis, int angle_deg, int duration_ms) {
    if (!s_initialized) {
        ESP_LOGW(TAG, "Servo not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (axis != SERVO_AXIS_X && axis != SERVO_AXIS_Y) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Validate angle range */
    if (angle_deg < 0 || angle_deg > 180) {
        return ESP_ERR_INVALID_ARG;
    }

    /* For zero duration, use immediate move */
    if (duration_ms <= 0) {
        return hal_servo_set_angle(axis, angle_deg);
    }

    /* Enqueue smooth move command */
    servo_cmd_msg_t cmd = {.type = CMD_TYPE_SINGLE,
                           .single = {.axis = axis, .angle_deg = angle_deg, .duration_ms = duration_ms}};

    /* Try to send, if queue full - drop oldest command and retry */
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        servo_cmd_msg_t dropped;
        if (xQueueReceive(s_cmd_queue, &dropped, 0) == pdTRUE) {
            ESP_LOGD(TAG, "Dropped old command to make room");
        }
        if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Command queue full even after drop");
            return ESP_ERR_TIMEOUT;
        }
    }

    ESP_LOGD(TAG, "Smooth move queued: axis=%s, angle=%d, duration=%dms", axis == SERVO_AXIS_X ? "X" : "Y", angle_deg,
             duration_ms);

    return ESP_OK;
}

esp_err_t hal_servo_move_sync(int x_deg, int y_deg, int duration_ms) {
    if (!s_initialized) {
        ESP_LOGW(TAG, "Servo not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Validate angles */
    if (x_deg < 0 || x_deg > 180 || y_deg < 0 || y_deg > 180) {
        return ESP_ERR_INVALID_ARG;
    }

    /* For zero duration, use immediate moves */
    if (duration_ms <= 0) {
        esp_err_t ret_x = hal_servo_set_angle(SERVO_AXIS_X, x_deg);
        esp_err_t ret_y = hal_servo_set_angle(SERVO_AXIS_Y, y_deg);
        return (ret_x != ESP_OK) ? ret_x : ret_y;
    }

    /* Enqueue synchronized move command */
    servo_cmd_msg_t cmd = {.type = CMD_TYPE_SYNC, .sync = {.x_deg = x_deg, .y_deg = y_deg, .duration_ms = duration_ms}};

    /* Try to send, if queue full - drop oldest command and retry */
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        servo_cmd_msg_t dropped;
        if (xQueueReceive(s_cmd_queue, &dropped, 0) == pdTRUE) {
            ESP_LOGD(TAG, "Dropped old command to make room for sync move");
        }
        if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Command queue full even after drop");
            return ESP_ERR_TIMEOUT;
        }
    }

    ESP_LOGD(TAG, "Sync move queued: X=%d, Y=%d, duration=%dms", x_deg, y_deg, duration_ms);

    return ESP_OK;
}

esp_err_t hal_servo_send_cmd(const char *id, int angle_deg, int duration_ms) {
    if (!id) {
        return ESP_ERR_INVALID_ARG;
    }

    servo_axis_t axis;
    char upper = (char)toupper((unsigned char)id[0]);

    if (upper == 'X') {
        axis = SERVO_AXIS_X;
    } else if (upper == 'Y') {
        axis = SERVO_AXIS_Y;
    } else {
        ESP_LOGW(TAG, "Unknown servo id: %s", id);
        return ESP_ERR_INVALID_ARG;
    }

    return hal_servo_move_smooth(axis, angle_deg, duration_ms);
}

int hal_servo_get_angle(servo_axis_t axis) {
    if (!s_initialized) {
        return -1;
    }

    if (axis != SERVO_AXIS_X && axis != SERVO_AXIS_Y) {
        return -1;
    }

    int angle = -1;
    if (xSemaphoreTake(s_angle_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        angle = s_angle[axis];
        xSemaphoreGive(s_angle_mutex);
    }

    return angle;
}
