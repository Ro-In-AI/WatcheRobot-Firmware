# ⚠️ DEPRECATED — MCU Firmware

**Status**: Deprecated as of 2026-03-11. No longer maintained.

---

## Why Deprecated

The secondary MCU (ESP32) was originally used as a dedicated servo controller, communicating with the main ESP32-S3 over UART.

**This architecture has been replaced** by direct GPIO PWM output from the ESP32-S3:

```
Before (deprecated):
  ESP32-S3 → UART → ESP32 MCU → LEDC PWM → Servos

After (current):
  ESP32-S3 → LEDC PWM (GPIO 19/20) → Servos
```

The `hal_servo` component in `firmware/s3/components/hal/hal_servo/` now handles servo control directly.

---

## Why Kept

This directory is preserved for historical reference:
- Documents the original dual-chip architecture
- Contains the servo smooth-move algorithm (`servo_control.c`) from which `hal_servo` was derived
- May be useful if a secondary MCU is needed in a future hardware revision

---

## Do Not

- Do not flash this firmware to any board
- Do not add new features here
- Do not reference this code from the S3 firmware
