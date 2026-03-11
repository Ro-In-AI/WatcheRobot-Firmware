/**
 * @file hal_servo.c
 * @brief Servo HAL stub — Phase 2 implementation pending
 *
 * Phase 1: logs calls and returns ESP_OK to allow boot without hardware.
 * Phase 2: replace with LEDC PWM + smooth-move FreeRTOS task.
 *
 * GPIO mapping (from CLAUDE.md):
 *   GPIO 19 → Servo X axis (LEDC channel 0)
 *   GPIO 20 → Servo Y axis (LEDC channel 1)
 */

#include "hal_servo.h"
#include "esp_log.h"
#include <string.h>
#include <ctype.h>

#define TAG "HAL_SERVO"

/* Current angles (degrees) */
static int s_angle[2] = {90, 90};  /* Default center position */
static bool s_initialized = false;

esp_err_t hal_servo_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    /* TODO (Phase 2): configure LEDC timer + channels on GPIO 19/20 */
    ESP_LOGI(TAG, "Servo HAL init (Phase 2 stub): GPIO 19 = X, GPIO 20 = Y");
    s_initialized = true;
    return ESP_OK;
}

esp_err_t hal_servo_set_angle(servo_axis_t axis, int angle_deg)
{
    if (axis != SERVO_AXIS_X && axis != SERVO_AXIS_Y) {
        return ESP_ERR_INVALID_ARG;
    }
    if (angle_deg < 0 || angle_deg > 180) {
        return ESP_ERR_INVALID_ARG;
    }
    s_angle[axis] = angle_deg;
    ESP_LOGI(TAG, "Set angle: axis=%s, angle=%d (stub)", axis == SERVO_AXIS_X ? "X" : "Y", angle_deg);
    /* TODO (Phase 2): write LEDC duty cycle */
    return ESP_OK;
}

esp_err_t hal_servo_move_smooth(servo_axis_t axis, int angle_deg, int duration_ms)
{
    if (axis != SERVO_AXIS_X && axis != SERVO_AXIS_Y) {
        return ESP_ERR_INVALID_ARG;
    }
    if (angle_deg < 0 || angle_deg > 180) {
        return ESP_ERR_INVALID_ARG;
    }
    s_angle[axis] = angle_deg;
    ESP_LOGI(TAG, "Smooth move: axis=%s, angle=%d, duration=%dms (stub)",
             axis == SERVO_AXIS_X ? "X" : "Y", angle_deg, duration_ms);
    /* TODO (Phase 2): enqueue to smooth-move task */
    return ESP_OK;
}

esp_err_t hal_servo_move_sync(int x_deg, int y_deg, int duration_ms)
{
    hal_servo_move_smooth(SERVO_AXIS_X, x_deg, duration_ms);
    hal_servo_move_smooth(SERVO_AXIS_Y, y_deg, duration_ms);
    return ESP_OK;
}

esp_err_t hal_servo_send_cmd(const char *id, int angle_deg, int duration_ms)
{
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

int hal_servo_get_angle(servo_axis_t axis)
{
    if (!s_initialized || (axis != SERVO_AXIS_X && axis != SERVO_AXIS_Y)) {
        return -1;
    }
    return s_angle[axis];
}
