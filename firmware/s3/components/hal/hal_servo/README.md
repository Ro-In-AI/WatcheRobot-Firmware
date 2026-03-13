# HAL Servo Component

Servo HAL for LEDC PWM direct drive on GPIO 19 (X-axis) and GPIO 20 (Y-axis).

## Overview

This component provides the hardware abstraction layer for dual-axis servo control on the WatcheRobot ESP32-S3 platform. It replaces the UART-to-MCU servo bridge used in v1.x with direct LEDC PWM control.

## Features

- **LEDC PWM Direct Drive**: 50Hz, 14-bit resolution PWM signals
- **Dual-Axis Control**: X-axis (pan, 0-180°) and Y-axis (tilt, configurable limits)
- **Smooth Movement**: Background FreeRTOS task with linear interpolation
- **Synchronized Motion**: Simultaneous dual-axis movement
- **Mechanical Protection**: Y-axis limits prevent hardware damage
- **Thread-Safe**: Mutex-protected angle access

## GPIO Mapping

| Axis | GPIO | LEDC Channel | Range |
|------|------|--------------|-------|
| X (pan) | 19 | LEDC_TIMER_0, CH0 | 0-180° |
| Y (tilt) | 20 | LEDC_TIMER_0, CH1 | 90-150° (configurable) |

## API Reference

### Initialization

```c
esp_err_t hal_servo_init(void);
```

Initialize LEDC timer/channels and start the smooth-move background task. Must be called before any other servo functions.

### Immediate Movement

```c
esp_err_t hal_servo_set_angle(servo_axis_t axis, int angle_deg);
```

Set servo angle immediately without smoothing.

- `axis`: `SERVO_AXIS_X` or `SERVO_AXIS_Y`
- `angle_deg`: Target angle (0-180°)
- Returns: `ESP_OK` on success, `ESP_ERR_INVALID_ARG` if angle out of range

### Smooth Movement

```c
esp_err_t hal_servo_move_smooth(servo_axis_t axis, int angle_deg, int duration_ms);
```

Move servo to angle with linear interpolation over specified duration.

- `axis`: `SERVO_AXIS_X` or `SERVO_AXIS_Y`
- `angle_deg`: Target angle (0-180°)
- `duration_ms`: Movement duration in milliseconds
- Returns: `ESP_OK` on success, `ESP_ERR_TIMEOUT` if queue is full

### Synchronized Movement

```c
esp_err_t hal_servo_move_sync(int x_deg, int y_deg, int duration_ms);
```

Move both axes simultaneously with coordinated timing.

- `x_deg`: X-axis target angle (0-180°)
- `y_deg`: Y-axis target angle (0-180°)
- `duration_ms`: Movement duration in milliseconds
- Returns: `ESP_OK` on success

### String-Based Command

```c
esp_err_t hal_servo_send_cmd(const char *id, int angle_deg, int duration_ms);
```

Convenience wrapper for WebSocket handlers. Maps "X"/"Y" strings to axis.

- `id`: Axis identifier ("X" or "Y", case-insensitive)
- `angle_deg`: Target angle
- `duration_ms`: Movement duration
- Returns: `ESP_OK` on success, `ESP_ERR_INVALID_ARG` if id unknown

### Query Current Position

```c
int hal_servo_get_angle(servo_axis_t axis);
```

Get current servo angle.

- Returns: Current angle in degrees, or -1 if not initialized

## Configuration (Kconfig)

| Option | Default | Description |
|--------|---------|-------------|
| `WATCHER_SERVO_X_GPIO` | 19 | X-axis PWM output GPIO |
| `WATCHER_SERVO_Y_GPIO` | 20 | Y-axis PWM output GPIO |
| `WATCHER_SERVO_Y_MIN_DEG` | 90 | Y-axis mechanical minimum |
| `WATCHER_SERVO_Y_MAX_DEG` | 150 | Y-axis mechanical maximum |
| `WATCHER_SERVO_SMOOTH_STEP_MS` | 10 | Interpolation step interval |

## PWM Timing

- **Frequency**: 50Hz (20ms period)
- **Resolution**: 14-bit (16384 levels)
- **Pulse Width**: 1ms (0°) to 2ms (180°)
- **Duty Cycle**: 819 (0°) to 1638 (180°)

## Usage Example

```c
#include "hal_servo.h"

void app_main(void)
{
    // Initialize servo HAL
    ESP_ERROR_CHECK(hal_servo_init());

    // Immediate move to center
    hal_servo_set_angle(SERVO_AXIS_X, 90);
    hal_servo_set_angle(SERVO_AXIS_Y, 120);

    // Smooth pan over 1 second
    hal_servo_move_smooth(SERVO_AXIS_X, 45, 1000);

    // Synchronized dual-axis move
    hal_servo_move_sync(90, 100, 500);

    // String-based command (for WebSocket handler)
    hal_servo_send_cmd("X", 135, 800);

    // Query current position
    int x_angle = hal_servo_get_angle(SERVO_AXIS_X);
}
```

## Dependencies

- `driver` (ESP-IDF LEDC driver)
- `freertos` (FreeRTOS for task and queue management)

## Thread Safety

- All public APIs are thread-safe
- Internal angle state protected by mutex
- Commands processed sequentially via FreeRTOS queue

## Mechanical Limits

The Y-axis has mechanical limits (default 90-150°) to prevent hardware damage. These are enforced automatically:

- `hal_servo_set_angle()`: Clamps Y-axis to limits
- `hal_servo_move_smooth()`: Clamps Y-axis target
- `hal_servo_move_sync()`: Clamps Y-axis target

Adjust limits via Kconfig if your hardware has different constraints.
