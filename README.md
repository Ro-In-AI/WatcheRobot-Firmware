# WatcheRobot Firmware

> ESP32-S3 based AI assistant robot firmware — voice interaction, servo control, LVGL display, BLE, and camera streaming.

[![Build](https://github.com/Ro-In-AI/WatcheRobot-Firmware/actions/workflows/build.yml/badge.svg)](https://github.com/Ro-In-AI/WatcheRobot-Firmware/actions/workflows/build.yml)
[![License: Apache-2.0](https://img.shields.io/badge/Firmware-Apache--2.0-blue.svg)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.2.1-green.svg)](https://docs.espressif.com/projects/esp-idf/en/v5.2.1/)
[![Target](https://img.shields.io/badge/Target-ESP32--S3-orange.svg)](https://www.espressif.com/en/products/socs/esp32-s3)

---

## What is WatcheRobot?

WatcheRobot is an open-source AI assistant robot built on the **SenseCAP Watcher** hardware (ESP32-S3 + Himax vision AI). It integrates:

- 🎙️ **Voice interaction** — offline wake word + cloud ASR/LLM/TTS pipeline
- 🤖 **Servo gimbal** — dual-axis direct GPIO PWM control (no secondary MCU)
- 📺 **LVGL display** — 30fps animated expressions (412×412 QSPI LCD)
- 📡 **BLE** — phone control + WiFi provisioning
- 📷 **Camera streaming** — Himax JPEG frames over WebSocket
- 🔄 **OTA** — firmware updates over HTTP, animation assets over WebSocket

### Current Release Track

- Current release target: `v0.1.3`
- `v0.1.3` is the recommended baseline for stable **BLE** and **Wi-Fi** testing on the current ESP32-S3 mainline:
  - BLE provisioning is stable enough for first-time Wi-Fi setup and credential recovery validation
  - BLE disconnect to Wi-Fi reconnect handoff is hardened for repeated local test cycles
  - Low-memory BLE-to-Wi-Fi recovery now pauses optional cloud runtime work before retrying Wi-Fi
  - Recording-time UI updates are guarded under low internal heap to reduce LCD flush failures during connectivity testing
  - WebSocket runtime memory usage is reduced so BLE / Wi-Fi bring-up is less likely to be blocked by internal heap pressure
  - Runtime input initialization still happens right after boot UI setup, and physical restart remains available with 3 short presses
- This release is suitable for repeated BLE pairing, Wi-Fi provisioning, disconnect/reconnect, and local multi-device regression testing
- Cloud voice throughput and end-to-end server-side interaction remain under active tuning, but BLE and Wi-Fi validation should use `v0.1.3` as the primary test baseline

---

## Hardware Requirements

| Component | Details |
|-----------|---------|
| **Main board** | SenseCAP Watcher (ESP32-S3, 16MB Flash, 8MB PSRAM) |
| **Display** | SPD2010 QSPI LCD, 412×412 |
| **Microphone** | I2S DMIC (onboard) |
| **Speaker** | I2S amplifier (onboard) |
| **Camera** | Himax HX6538 (onboard, via SSCMA protocol) |
| **Servos** | Standard PWM servo × 2, connected to GPIO 19 (X) and GPIO 20 (Y) |
| **Cloud server** | PC running `watcher-server` (Python WebSocket) |

---

## Quick Start

### 1. Prerequisites

```powershell
# Install ESP-IDF v5.2.1
# https://docs.espressif.com/projects/esp-idf/en/v5.2.1/get-started/

# Activate ESP-IDF environment (Windows PowerShell)
C:\Espressif\frameworks\esp-idf-v5.2.1\export.ps1
```

### 2. Clone & Configure

```bash
git clone https://github.com/Ro-In-AI/WatcheRobot-Firmware.git
cd WatcheRobot-Firmware/firmware/s3

# Edit server settings if needed. Wi-Fi credentials are no longer baked into firmware;
# first-time setup should use BLE provisioning or previously stored STA credentials.
idf.py menuconfig
# Navigate: Watcher Configuration → Server / BLE
```

### 3. Build & Flash

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

### 4. Start the Cloud Server

```bash
# In a separate terminal (Python 3.10+)
cd watcher-server      # see: https://github.com/Ro-In-AI/watcher-server
pip install -r requirements.txt
python src/main.py
```

> The device auto-discovers the server via UDP broadcast on the local network.

---

## Architecture Overview

```
┌────────────────────────────────────────────────────┐
│              ESP32-S3 (WatcheRobot Firmware)        │
│                                                     │
│  services/  voice_service  anim_service  ota_service│
│             camera_service                          │
│  protocols/ ws_client      discovery    ble_service │
│  hal/       hal_audio      hal_display  hal_servo   │
│             hal_camera                              │
│  drivers/   bsp_watcher                             │
└──────────────────┬─────────────────────────────────┘
                   │ WebSocket (WiFi)
                   ▼
┌────────────────────────────────────────────────────┐
│              Cloud Server (Python)                  │
│   ASR (Aliyun) → LLM (Claude) → TTS (Volcengine)  │
└────────────────────────────────────────────────────┘
```

See [docs/architecture.md](docs/architecture.md) for the full system design.

---

## Repository Structure

```
WatcheRobot-Firmware/
├── firmware/s3/           # ESP32-S3 firmware (main development)
│   ├── main/              # App entry point (≤100 lines)
│   └── components/        # Layered components (drivers/hal/protocols/services/utils)
├── firmware/mcu/          # ⚠️ DEPRECATED — kept for reference only
├── hardware/              # Hardware design files (KiCad)
├── docs/                  # Documentation
├── tools/                 # PC-side helper scripts
└── .github/               # CI/CD and issue templates
```

---

## Documentation

### Getting Started

| Document | Description |
|----------|-------------|
| [docs/getting-started.md](docs/getting-started.md) | Full setup guide |
| [docs/roadmap.md](docs/roadmap.md) | Development phases and milestones |

### Architecture & Design

| Document | Description |
|----------|-------------|
| [docs/architecture.md](docs/architecture.md) | System architecture & component design |
| [docs/hardware/gpio-mapping.md](docs/hardware/gpio-mapping.md) | GPIO pin assignment |
| [docs/hardware/bom.md](docs/hardware/bom.md) | Bill of materials |

### Protocols

| Document | Description |
|----------|-------------|
| [docs/protocol/websocket-protocol.md](docs/protocol/websocket-protocol.md) | WebSocket protocol v2.3 |
| [docs/protocol/voice-stream.md](docs/protocol/voice-stream.md) | Audio streaming specification |

### Development

| Document | Description |
|----------|-------------|
| [docs/development/known-issues.md](docs/development/known-issues.md) | Known issues & workarounds |
| [docs/development/testing.md](docs/development/testing.md) | Testing guide |
| [docs/development/codex-multi-device-workflow.md](docs/development/codex-multi-device-workflow.md) | Codex lane workflow for multi-feature / multi-device development |
| [CONTRIBUTING.md](CONTRIBUTING.md) | How to contribute |
| [CHANGELOG.md](CHANGELOG.md) | Version history |

---

## License

- **Firmware**: [Apache License 2.0](LICENSE)
- **Hardware designs**: [CERN-OHL-P 2.0](hardware/LICENSE) *(permissive open hardware)*

---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for development environment setup, code style, and PR process.
