# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

---

## Project Overview

WatcheRobot is an ESP32-S3 based AI assistant robot firmware with voice interaction, servo control, LVGL display, BLE, and camera streaming. Built on SenseCAP Watcher hardware.

---

## Build Commands

```bash
# Activate ESP-IDF environment first (Windows PowerShell)
C:\Espressif\frameworks\esp-idf-v5.2.1\export.ps1

# Navigate to firmware directory
cd firmware/s3

# Set target and build
idf.py set-target esp32s3
idf.py build

# Flash and monitor
idf.py -p COM3 flash monitor    # Windows
idf.py -p /dev/ttyUSB0 flash monitor  # Linux

# Configuration menu
idf.py menuconfig
# Navigate: Watcher Configuration → WiFi / Server / HAL settings

# Clean build
idf.py fullclean && idf.py build
```

---

## Architecture: Four-Layer Component Model

```
firmware/s3/components/
├── drivers/        # Layer 1 — BSP (sensecap-watcher SDK wrapper)
├── hal/            # Layer 2 — Hardware Abstraction (audio, display, servo, camera, button)
├── protocols/      # Layer 3 — Communication (ws_client, discovery, ble_service)
├── services/       # Layer 4 — Business Logic (voice, anim, camera, ota)
└── utils/          # Cross-layer utilities (wifi_manager, boot_anim)
```

### Layer Dependency Rules (Strictly Enforced)

| Layer | Can depend on | Cannot depend on |
|-------|--------------|-----------------|
| `services/` | `hal/`, `protocols/`, `utils/` | other `services/` |
| `protocols/` | `hal/`, `utils/` | `services/` |
| `hal/` | `drivers/`, `utils/` | `protocols/`, `services/` |
| `drivers/` | ESP-IDF only | any custom component |
| `utils/` | ESP-IDF only | any custom component |

---

## Component Structure Requirements

Every component must include:

```
my_component/
├── CMakeLists.txt         # Required
├── idf_component.yml      # Required — version, description, license
├── Kconfig                # Required — all configurable params
├── README.md              # Required — purpose, API summary
├── CHANGELOG.md           # Required — version history
├── include/               # Public headers with Doxygen
└── src/                   # Implementation
```

---

## Code Style

### Naming Conventions

| Item | Convention | Example |
|------|-----------|---------|
| Functions | `{component}_{verb}_{noun}` | `hal_servo_set_angle()` |
| Types | `{component}_{name}_t` | `servo_axis_t` |
| Constants | `UPPER_SNAKE_CASE` | `SERVO_Y_MIN_DEG` |
| Kconfig | `WATCHER_{COMPONENT}_{PARAM}` | `WATCHER_SERVO_X_GPIO` |

### Formatting

```bash
# Format with clang-format
clang-format -i firmware/s3/components/hal/hal_servo/src/hal_servo.c

# Check formatting
clang-format --dry-run --Werror firmware/s3/components/**/*.c
```

### Error Handling

- All functions return `esp_err_t`
- Use `ESP_ERROR_CHECK()` for critical paths
- Never silently ignore errors

---

## Key Hardware Configuration

| Component | GPIO | Notes |
|-----------|------|-------|
| Servo X-axis | 19 | LEDC PWM, 0–180° |
| Servo Y-axis | 20 | LEDC PWM, 90–150° (mechanical limit) |
| Audio | I2S | Managed by sensecap-watcher BSP |
| Display | QSPI | SPD2010 412×412 LCD |
| Camera | SPI/UART | Himax HX6538 via SSCMA protocol |

---

## WebSocket Protocol

### Downlink (Server → Device)

| `type` | Description |
|--------|-------------|
| `servo` | Move servo: `{"id":"X","angle":90,"time":500}` |
| `display` | Update display: `{"text":"...","emoji":"speaking"}` |
| `camera_start` / `camera_stop` | Video stream control |
| `fw_ota_notify` | OTA trigger with URL + SHA256 |

### Uplink (Device → Server)

| `type` | Description |
|--------|-------------|
| `audio_end` | Recording complete |
| `status` | State report |
| `device_info` | On-connect: firmware version, hardware ID |

Binary frames: PCM audio (16kHz uplink, 24kHz downlink), WVID video format.

---

## Commit Message Format

Conventional Commits with component scope:

```
<type>(<scope>): <description>

# Examples:
feat(hal_servo): add synchronized dual-axis smooth move
fix(anim_service): release PSRAM buffer before loading new animation
refactor(ws_client): extract protocol parsing into ws_protocol.c
docs(hal_camera): add Doxygen for capture_once API
```

Types: `feat` `fix` `refactor` `docs` `test` `chore` `perf` `ci`

---

## Memory Layout

- Flash: 16MB total
- Firmware partitions: 2× 5MB (ota_0, ota_1)
- SPIFFS storage: 5.5MB (animation assets + OTA buffer)
- PSRAM: 8MB (LVGL frame buffers, decoded PNGs)

---

## Related Documentation

- [docs/architecture.md](docs/architecture.md) — Full system design
- [docs/getting-started.md](docs/getting-started.md) — Setup guide
- [docs/hardware/gpio-mapping.md](docs/hardware/gpio-mapping.md) — Pin reference
- [CONTRIBUTING.md](CONTRIBUTING.md) — Development guidelines
