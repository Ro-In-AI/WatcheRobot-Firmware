# WatcheRobot Firmware Memory

Persistent context for Claude Code sessions.

---

## Project Identity

- **Name**: WatcheRobot Firmware
- **Target**: ESP32-S3 (SenseCAP Watcher hardware)
- **Framework**: ESP-IDF v5.2.1
- **Language**: C (embedded)

---

## Architecture Summary

Four-layer component model with strict dependency rules:

```
services/  →  protocols/  →  hal/  →  drivers/
    ↓              ↓          ↓         ↓
   utils/        utils/     utils/   ESP-IDF
```

**Key Components**:
- Drivers: `bsp_watcher` (sensecap-watcher SDK wrapper)
- HAL: `hal_audio`, `hal_display`, `hal_servo`, `hal_camera`, `hal_button`
- Services: `voice_service`, `anim_service`, `camera_service`, `ota_service`
- Protocols: `ws_client`, `discovery`, `ble_service`
- Utils: `wifi_manager`, `boot_anim`

---

## Build Workflow

1. Activate ESP-IDF: `C:\Espressif\frameworks\esp-idf-v5.2.1\export.ps1`
2. `cd firmware/s3`
3. `idf.py set-target esp32s3`
4. `idf.py build`
5. `idf.py -p COM3 flash monitor`

---

## Hardware Notes

- Servo X: GPIO 19 (0–180°)
- Servo Y: GPIO 20 (90–150° mechanical limit)
- Display: SPD2010 QSPI 412×412
- Camera: Himax HX6538 (SSCMA protocol)
- Audio: I2S DMIC + I2S speaker

---

## Code Patterns

- Functions: `{component}_{verb}_{noun}` (e.g., `hal_servo_set_angle`)
- Types: `{component}_{name}_t`
- All functions return `esp_err_t`
- All params via Kconfig (no hardcoding)

---

## Session History

| Date | Summary |
|------|---------|
| 2026-03-11 | Session resume, documentation review, memory files updated |
| 2026-03-11 | Phase 1 component architecture migration completed |
