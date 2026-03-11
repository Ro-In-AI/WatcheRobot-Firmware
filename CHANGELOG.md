# Changelog

All notable changes to WatcheRobot Firmware are documented here.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

### Added
- Initial repository structure (migrated from MVP-W prototype)
- Four-layer ESP-IDF component architecture (`drivers/hal/protocols/services/utils`)
- `hal_servo` component: direct GPIO 19/20 LEDC PWM servo control (replaces UART→MCU bridge)
- `hal_camera` component: SSCMA client wrapper for Himax JPEG frame extraction
- `anim_service` component: 30fps animation engine (PNG→RGB565 PSRAM, `lv_animimg`)
- `ble_service` component: BLE GATT control + WiFi Provisioning
- `ota_service` component: firmware OTA via HTTP + WebSocket notification
- `camera_service` component: WebSocket video stream (WVID binary frame format)
- Dual OTA partition layout (`ota_0` / `ota_1`, 5MB each)
- WebSocket protocol v2.3 (servo / display / camera / OTA / BLE messages)
- GitHub Actions CI (`esp32s3` build on push/PR)
- `.clang-format` code style configuration
- Documentation structure (`docs/`, `CONTRIBUTING.md`, issue templates)

### Changed
- Architecture: single-chip (ESP32-S3 only), secondary MCU removed
- Servo control: UART bridge → GPIO direct PWM (`hal_servo`)
- Animation: PNG runtime decode → load-time decode to RGB565 PSRAM buffer
- `cJSON`: removed local copy, use ESP-IDF built-in `json` component

### Removed
- `uart_bridge` module (UART to MCU communication)
- `firmware/mcu/` active development (marked DEPRECATED)

---

## [1.x.x] — MVP-W Prototype (Internal)

MVP-W prototype phase completed. All core features working:
- End-to-end voice pipeline (wake word → ASR → LLM → TTS)
- PNG animation display (LVGL, ~6.7fps)
- S3 ↔ MCU UART servo control
- WebSocket v2.0 protocol
- UDP service discovery
