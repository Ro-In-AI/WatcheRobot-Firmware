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

> **⚠️ Claude must NOT run `idf.py` commands — Git Bash/MSys is incompatible with ESP-IDF.**
> User must run builds manually in PowerShell or ESP-IDF terminal.

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

## Dependency Version Policy

**SenseCAP Watcher SDK 依赖锁定 — 不升级**

sensecap-watcher SDK 使用 button v3.x API (`BUTTON_TYPE_CUSTOM`)，与 button v4.x 不兼容。
升级到 button v4 需要重写 SDK 内部代码，风险高且维护困难。

**锁定版本:**
- `espressif/button`: `~3.2.3` (NOT v4.x)
- `espressif/esp_lvgl_port`: `~1.4.0` (NOT v2.x — requires button v4)
- `lvgl/lvgl`: `~8.4.0` (compatible with esp_lvgl_port v1.x)

**原因:**
- button v4 移除了 `BUTTON_TYPE_CUSTOM`，改用 `button_driver_t` 驱动抽象
- esp_lvgl_port v2.x 依赖 button v4.x
- sensecap-watcher 是第三方 SDK，不应修改其内部实现

---

## Session History

| Date | Summary |
|------|---------|
| 2026-03-12 | 决定锁定 sensecap-watcher 依赖版本，不升级 button/LVGL |
| 2026-03-11 | Session resume, documentation review, memory files updated |
| 2026-03-11 | Phase 1 component architecture migration completed |
