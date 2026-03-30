# Changelog

All notable changes to WatcheRobot Firmware are documented here.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

---

## [0.0.91] - 2026-03-30

### Changed
- Removed the boot-time hardcoded hidden Wi-Fi credentials from firmware startup
- Boot now uses only stored STA credentials and otherwise waits for BLE provisioning to provide Wi-Fi

### Notes
- Release focus: Wi-Fi configuration cleanup for local device testing and BLE provisioning validation

---

## [0.0.9] - 2026-03-29

### Fixed
- WebSocket TTS playback now reassembles fragmented text and binary frames instead of dropping fragmented cloud audio
- Voice recording refuses to start until the cloud WebSocket session is ready, avoiding full-buffer upload failures before hello acknowledgement

### Changed
- WebSocket send failures now log the blocked state (`socket_connected` / `hello_ack`) to make cloud audio regressions easier to diagnose

### Notes
- Release focus: audio reliability for cloud ASR/TTS bring-up on the Watcher S3 test rigs

---

## [0.0.8] - 2026-03-29

### Added
- SPIFFS-based behavior action executor for core runtime states
- Watcher0327 animation asset pack and updated generated animation resources

### Changed
- Startup flow now integrates local SFX playback, behavior actions, and richer state transitions
- Animation playback is more stable across state updates and boot-time display transitions

### Notes
- This release bundles two major features:
  1. behavior action execution from SPIFFS
  2. upgraded animation/media asset pipeline
- Cross-feature integration is not fully validated yet; `0.1.0` is reserved for the fully integrated milestone

---

## [0.0.6] - 2026-03-27

### Added
- Codex multi-device flash workflow for local three-device automation testing
- Device alias templates and lane metadata/log conventions for repeatable local test runs

### Changed
- `flash-monitor.ps1` now supports device alias resolution, bounded monitor sessions, and dedicated per-device build directories
- Input initialization is delayed until cloud ready to make boot-time behavior more deterministic during test runs

### Notes
- Release target: Bluetooth provisioning validation and local automated device testing

---

## [0.0.4] - 2026-03-17

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
- Frozen S3 communication protocol baseline document for integration/regression handoff
- Firmware release version `0.0.4`

### Changed
- Architecture: single-chip (ESP32-S3 only), secondary MCU removed
- Servo control: UART bridge → GPIO direct PWM (`hal_servo`)
- Animation: PNG runtime decode → load-time decode to RGB565 PSRAM buffer
- `cJSON`: removed local copy, use ESP-IDF built-in `json` component
- Default Wi-Fi credentials updated to `Erroright / erroright`
- OTA firmware version reporting now follows the ESP-IDF project version

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
