/**
 * @file bsp_watcher.h
 * @brief Watcher S3 Board Support Package
 *
 * Wraps sensecap-watcher SDK, provides centralized GPIO definitions.
 */

#ifndef BSP_WATCHER_H
#define BSP_WATCHER_H

#include "sensecap-watcher.h"

/* Servo GPIO (repurposed from UART) */
#define BSP_SERVO_X_GPIO    CONFIG_WATCHER_SERVO_X_GPIO
#define BSP_SERVO_Y_GPIO    CONFIG_WATCHER_SERVO_Y_GPIO

/* Button GPIO (managed by SDK) */
#define BSP_BUTTON_GPIO     41  /* Encoder push-button */

esp_err_t bsp_board_init(void);
i2c_port_t bsp_i2c_get_port(void);

#endif /* BSP_WATCHER_H */
