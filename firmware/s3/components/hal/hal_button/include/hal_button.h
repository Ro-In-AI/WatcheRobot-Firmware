#ifndef HAL_BUTTON_H
#define HAL_BUTTON_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Button event callback
 * @param pressed true if button pressed, false if released
 */
typedef void (*button_callback_t)(bool pressed);

/**
 * Initialize button GPIO (encoder button on GPIO 41)
 * @param callback Function to call on button press/release
 * @return 0 on success, -1 on error
 */
int hal_button_init(button_callback_t callback);

/**
 * Check if button is currently pressed
 * @return true if pressed, false if not
 */
bool hal_button_is_pressed(void);

/**
 * Poll button state (call from task context)
 * This reads the button via I2C and calls callback on change
 */
void hal_button_poll(void);

/**
 * Deinitialize button GPIO
 */
void hal_button_deinit(void);

#endif /* HAL_BUTTON_H */
