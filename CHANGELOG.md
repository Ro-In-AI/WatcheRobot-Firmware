# Changelog

All notable changes to WatcheRobot Firmware are documented here.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

---

## [0.1.3] - 2026-04-02

### Added
- Staged low-memory BLE-to-Wi-Fi recovery flow that can pause BLE advertising, reclaim optional cloud runtime memory, and retry Wi-Fi with a degraded largest-block threshold after repeated heap defers

### Fixed
- BLE and Wi-Fi recovery is more stable on low-internal-heap devices after BLE disconnects
- Recording start is less likely to trigger LCD flush `ESP_ERR_NO_MEM` failures when internal heap headroom is too small
- WebSocket runtime teardown now releases more resources cleanly before reconnect attempts

### Changed
- Firmware version is now tracked as `v0.1.3`
- WebSocket client runtime now uses smaller default buffers, dynamic client buffering, and PSRAM-backed audio frame storage to preserve internal RAM for BLE, Wi-Fi, and LCD DMA
- Listening animation assets were refreshed and the generated SPIFFS animation manifest was updated

### Notes
- Release focus: stable BLE provisioning and Wi-Fi validation on the current mainline
- `v0.1.3` is the recommended baseline for repeated BLE pairing, Wi-Fi provisioning, disconnect/reconnect, and recovery-path testing

---

## [0.1.2alpha] - 2026-04-01

### Added
- BLE-priority transport coordinator for cloud recovery after BLE sessions release control

### Fixed
- BLE / Wi-Fi / WebSocket handoff is more stable after BLE disconnects
- Cloud recovery path is less fragile during provisioning, reconnect, and local-control-first bring-up

### Changed
- Firmware version is now tracked as `0.1.2alpha`
- Main transport flow now prioritizes BLE control sessions while coordinating Wi-Fi resume, discovery, and WebSocket recovery in the background
- Current mainline also includes the earlier 3-click restart path and delayed input initialization improvements from `0.1.1`

### Notes
- Release focus: BLE-priority transport recovery and continued local bring-up validation

---

## [0.1.1] - 2026-04-01

### Added
- Earlier physical restart availability through delayed input initialization immediately after boot UI setup

### Fixed
- Button-based restart no longer depends on cloud readiness before becoming available
- Boot-time input probing now emits clearer diagnostics around IO expander readiness and delayed input attachment

### Changed
- Physical restart trigger is reduced from 5 short presses to 3 short presses
- Runtime input initialization and restart callback registration now run after boot UI setup regardless of cloud connection state

### Notes
- Release focus: make local recovery and BLE-side bring-up easier during ongoing integration and provisioning validation

---

## [0.1.0-beta] - 2026-03-30

### Added
- Beta release track for the current integrated `main` branch
- SPIFFS-driven behavior actions with guarded fallback for local expression execution
- Multi-device Codex lane flashing workflow for local parallel bring-up and repeated device testing

### Fixed
- Startup display sequencing and state handoff during cloud bring-up are more stable
- BLE provisioning path is hardened for first-time network setup and recovery after Wi-Fi cleanup
- WebSocket recording/upload path is more resilient during long audio capture and cloud handoff
- Local SFX playback now hands off to cloud TTS more cleanly during reply start

### Changed
- Firmware version is now tracked as `0.1.0-beta` for the current integration milestone
- Mainline audio path now favors smoother local SFX playback, safer TTS takeover, and more tolerant WebSocket timing
- Behavior state transitions now integrate animation, SPIFFS action playback, and safer fallback handling
- Local multi-device development workflow is now part of the beta bring-up path for `s3-a / s3-b / s3-c / s3-d` style rigs

### Summary
- This beta marks the first mainline milestone where voice, BLE provisioning, animation, local actions, and automated multi-device testing are all available together on the current integrated branch.
- The branch is suitable for ongoing joint debugging and integration validation, while the later stable `0.1.0` release remains reserved for fully completed end-to-end bring-up.

---

## [0.0.92] - 2026-03-30

### Fixed
- Local SFX playback now hands the audio path cleanly to cloud TTS instead of briefly switching back to recording mode during reply start
- Recording upload no longer drops the WebSocket connection as easily under low-memory UI load

### Changed
- Local SFX now prefers PSRAM prefetch with larger playback chunks to reduce SPIFFS read jitter and short stutters
- WebSocket TTS playback now streams fragmented binary audio frames directly and keeps fragmented text/binary state across packets
- Boot-time discovery now uses a bounded timeout so the device can continue startup in BLE-only mode when the cloud server is unavailable
- WebSocket recording/upload path now uses longer send/network timeouts, higher client task priority, keep-alive, and richer error diagnostics
- Listening UI is frozen to a static frame during recording to reduce animation pressure while voice data is uploading

### Notes
- Release focus: audio playback smoothness, TTS start handoff, and WebSocket stability during voice recording

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
