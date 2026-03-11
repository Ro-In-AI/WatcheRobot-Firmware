# GPIO Pin Mapping

> SenseCAP Watcher — ESP32-S3 GPIO assignment for WatcheRobot Firmware

---

## Servo Control (Direct LEDC PWM)

| GPIO | Function | Direction | Notes |
|------|----------|-----------|-------|
| **19** | Servo X-axis PWM | OUT | Left/right, 0–180°. Previously UART TX to MCU (MCU removed). |
| **20** | Servo Y-axis PWM | OUT | Up/down, 90–150° (mechanical limit). Previously UART RX from MCU. |

LEDC configuration:
- Timer: LEDC_TIMER_0, 50Hz, 14-bit resolution
- Channel 0 → GPIO 19 (X-axis)
- Channel 1 → GPIO 20 (Y-axis)

---

## Audio

| GPIO | Function | Direction | Notes |
|------|----------|-----------|-------|
| See sdkconfig | I2S DMIC Clock | OUT | Microphone clock |
| See sdkconfig | I2S DMIC Data | IN | Microphone data |
| See sdkconfig | I2S Speaker BCLK | OUT | Speaker bit clock |
| See sdkconfig | I2S Speaker LRCK | OUT | Speaker word select |
| See sdkconfig | I2S Speaker Data | OUT | Speaker data |

> Exact pins are managed by the `sensecap-watcher` BSP SDK. See `components/sensecap-watcher/` for definitions.

---

## Display (QSPI LCD)

| GPIO | Function | Notes |
|------|----------|-------|
| See sensecap-watcher SDK | SPD2010 QSPI | 412×412 LCD, managed by BSP |

---

## Camera (Himax HX6538)

Communication with the Himax vision AI chip uses the SSCMA protocol over SPI/UART. GPIO assignment is managed by the `sscma_client` component and `sensecap-watcher` BSP.

---

## Button

| GPIO | Function | Notes |
|------|----------|-------|
| See sensecap-watcher SDK | Physical button | Long press = start recording, 5-click = reboot |

---

## Kconfig Overrides

The following GPIOs are configurable via `idf.py menuconfig` → **Watcher HAL**:

| Kconfig Symbol | Default | Description |
|----------------|---------|-------------|
| `WATCHER_SERVO_X_GPIO` | 19 | X-axis servo PWM GPIO |
| `WATCHER_SERVO_Y_GPIO` | 20 | Y-axis servo PWM GPIO |
| `WATCHER_SERVO_Y_MIN_DEG` | 90 | Y-axis lower mechanical limit |
| `WATCHER_SERVO_Y_MAX_DEG` | 150 | Y-axis upper mechanical limit |

All other GPIO assignments are fixed by the SenseCAP Watcher hardware and managed by the BSP.
