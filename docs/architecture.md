# System Architecture

> WatcheRobot Firmware v2.0 — Four-layer ESP-IDF component architecture

---

## System Overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        WatcheRobot Hardware                              │
│                                                                         │
│  Microphone (I2S)    Speaker (I2S)    Himax Camera     LCD (QSPI)      │
│       │                   │               │                │            │
│       └───────────────────┴───────────────┴────────────────┘            │
│                                    │                                    │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │                    ESP32-S3 (WatcheRobot Firmware)              │    │
│  │                                                                 │    │
│  │  ┌───────────────────────────────────────────────────────────┐  │    │
│  │  │  services/   voice_service  anim_service  ota_service     │  │    │
│  │  │              camera_service                               │  │    │
│  │  ├───────────────────────────────────────────────────────────┤  │    │
│  │  │  protocols/  ws_client      discovery      ble_service    │  │    │
│  │  ├───────────────────────────────────────────────────────────┤  │    │
│  │  │  hal/        hal_audio  hal_display  hal_servo  hal_camera│  │    │
│  │  ├───────────────────────────────────────────────────────────┤  │    │
│  │  │  drivers/    bsp_watcher                                   │  │    │
│  │  └───────────────────────────────────────────────────────────┘  │    │
│  │                           │                                     │    │
│  │    GPIO 19 → Servo X      │ WebSocket / BLE                    │    │
│  │    GPIO 20 → Servo Y      │                                     │    │
│  └───────────────────────────┼─────────────────────────────────────┘    │
└──────────────────────────────┼──────────────────────────────────────────┘
                               │ WiFi
                               ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                         Cloud Server (Python)                            │
│   ASR (Aliyun) ──► LLM (Claude API) ──► Agent ──► TTS (Volcengine)     │
│   WebSocket Server   Service Discovery (UDP)   HTTP Firmware Server     │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## Component Layer Architecture

### Layer Rules (strictly enforced)

| Layer | Can depend on | Cannot depend on |
|-------|--------------|-----------------|
| `services/` | `hal/`, `protocols/`, `utils/` | other `services/` |
| `protocols/` | `hal/`, `utils/` | `services/` |
| `hal/` | `drivers/`, `utils/` | `protocols/`, `services/` |
| `drivers/` | ESP-IDF built-ins only | any custom component |
| `utils/` | ESP-IDF built-ins only | any custom component |

---

## Component Reference

### `drivers/bsp_watcher` — Board Support Package

Wraps the `sensecap-watcher` SDK and centralizes all board-level pin definitions.

```c
// Single source of truth for hardware mapping
#define BSP_SERVO_X_GPIO    19   // Kconfig: WATCHER_SERVO_X_GPIO
#define BSP_SERVO_Y_GPIO    20   // Kconfig: WATCHER_SERVO_Y_GPIO
```

---

### `hal/hal_audio` — Audio HAL

I2S abstraction for microphone (recording, 16kHz) and speaker (playback, 24kHz).
Sample rate switching is handled internally; callers use a unified `hal_audio_read/write` API.

---

### `hal/hal_display` — Display HAL

SPD2010 QSPI LCD initialization and LVGL port configuration. Provides the LVGL display handle used by `anim_service`.

---

### `hal/hal_servo` — Servo HAL *(new)*

Direct LEDC PWM control of two servo axes via GPIO 19 (X) and GPIO 20 (Y).
Replaces the previous UART→MCU bridge approach.

Features:
- Smooth move (background FreeRTOS task, 10ms step)
- Synchronized dual-axis move (both axes arrive at target simultaneously)
- Y-axis mechanical protection limits (Kconfig configurable)

```
GPIO 19 → LEDC CH0 → Servo X (left/right, 0–180°)
GPIO 20 → LEDC CH1 → Servo Y (up/down, 90–150° mechanical limit)
```

---

### `hal/hal_camera` — Camera HAL *(new)*

Wraps the `sscma_client` component to provide a simple JPEG frame callback interface to the Himax HX6538 vision AI chip.

```
Himax HX6538 ──[SSCMA/SPI]──► sscma_client ──► hal_camera ──► JPEG frame callback
```

---

### `protocols/ws_client` — WebSocket Client

WebSocket connection management, auto-reconnect, and message routing.
Handles both JSON control messages and binary audio/video frames.

Sub-modules:
- `ws_client.c` — connection lifecycle, TTS binary frame reception
- `ws_router.c` — message type dispatch
- `ws_handlers.c` — per-message-type handlers
- `ws_protocol.c` — serialization/deserialization (uses ESP-IDF `json` component)

---

### `protocols/discovery` — UDP Service Discovery

Broadcasts a UDP packet to discover the cloud server on the local network.
Device sends → server responds with IP:port → device connects WebSocket.

---

### `protocols/ble_service` — BLE Service *(new)*

Two BLE services running simultaneously:

1. **GATT Control Service** (UUID 0x1234): servo control, display update, device status notify
2. **WiFi Provisioning Service**: ESP-IDF `wifi_provisioning` over BLE

WiFi + BLE coexistence: enabled via `CONFIG_ESP32_WIFI_SW_COEXIST_ENABLE`.

---

### `services/voice_service` — Voice Service

State machine managing the full voice interaction lifecycle:
- Idle → wake word detected / button pressed → recording → sent → response
- Resume wake word detection after TTS playback completes

Sub-modules:
- `voice_fsm.c` — state machine (formerly `button_voice.c`)
- `wake_word.c` — ESP-SR AFE wrapper (formerly `hal_wake_word.c`)

---

### `services/anim_service` — Animation Service

30fps LVGL animation engine.

**Pipeline:**
```
SPIFFS PNG files
      │ (on animation type switch — one-time decode)
      ▼
LVGL PNG decoder → RGB565 pixel buffers in PSRAM
      │ (during playback — pointer swap only, < 1ms)
      ▼
lv_animimg widget → LCD (30fps)
```

**Memory per frame:** 412 × 412 × 2 bytes (RGB565) = 332 KB
**Strategy:** Lazy load — only current animation type's frames kept in PSRAM.

Animation metadata (`/spiffs/anim/anim_meta.json`):
```json
{
  "version": "1.0",
  "fps": 30,
  "animations": {
    "speaking":  {"frames": 10, "loop": true},
    "listening": {"frames": 8,  "loop": true},
    "analyzing": {"frames": 9,  "loop": true},
    "standby":   {"frames": 6,  "loop": true},
    "greeting":  {"frames": 12, "loop": false}
  }
}
```

---

### `services/camera_service` — Camera Service *(new)*

Orchestrates video streaming from `hal_camera` over WebSocket.

**Priority rules:**
- TTS playback active → stream paused (audio priority)
- Voice recording active → stream throttled to 1fps
- WebSocket disconnected → stream stopped (not auto-resumed)

**Binary frame format (`WVID`):**
```
Offset  Size  Field
0       4     Magic: "WVID"
4       4     Timestamp (ms, uint32 big-endian)
8       2     Width (pixels)
10      2     Height (pixels)
12      4     JPEG size (bytes)
16      N     JPEG data
```

---

### `services/ota_service` — OTA Service *(new)*

Firmware update via HTTP download + WebSocket notification trigger.

**Flow:**
1. Server sends `fw_ota_notify` JSON with URL + SHA256
2. Device downloads firmware via `esp_https_ota` to `ota_1` partition
3. Device validates SHA256, sets boot partition to `ota_1`, reboots
4. Bootloader boots `ota_1`; on success, `ota_service_mark_valid()` prevents rollback
5. On crash: bootloader auto-rollback to `ota_0`

---

### `utils/wifi_manager` — WiFi Manager

WiFi connection and automatic reconnect logic. Wraps `esp_wifi` event handling.

---

### `utils/boot_anim` — Boot Animation

Arc progress bar displayed during startup (0–100%). Used by `app_main.c` to show initialization progress.

---

## Partition Table

```
# Name,    Type, SubType,  Offset,    Size      Description
nvs,       data, nvs,      0x9000,    0x6000    24KB   NVS storage
phy_init,  data, phy,      0xF000,    0x1000    4KB    RF calibration
otadata,   data, ota,      0x10000,   0x2000    8KB    OTA boot selection
ota_0,     app,  ota_0,    0x20000,   0x500000  5MB    Firmware partition A
ota_1,     app,  ota_1,    0x520000,  0x500000  5MB    Firmware partition B (OTA)
model,     data, spiffs,   0xA20000,  0x50000   320KB  ESP-SR wake word model
storage,   data, spiffs,   0xA70000,  0x580000  5.5MB  Animation assets + OTA buffer
                                                15.9MB Total (fits 16MB flash)
```

---

## WebSocket Protocol v2.3

### Downlink (Server → Device)

| `type` | Payload | Description |
|--------|---------|-------------|
| `servo` | `{"id":"X","angle":90,"time":500}` | Move servo axis |
| `display` | `{"text":"...","emoji":"speaking"}` | Update display |
| `status` | `{"data":"processing"}` | State sync |
| `camera_start` | `{"fps":5}` | Start video stream |
| `camera_stop` | `{}` | Stop video stream |
| `camera_capture` | `{}` | Single frame capture |
| `fw_ota_notify` | `{"version":"2.1.0","url":"/fw/...","sha256":"..."}` | OTA trigger |
| `anim_ota_start` | `{"anim_name":"speaking","frame_count":10,...}` | Animation OTA |
| `reboot` | `{}` | Remote restart |

### Uplink (Device → Server)

| `type` | Payload | Description |
|--------|---------|-------------|
| `audio_end` | `{}` | Recording complete |
| `status` | `{"state":"listening"}` | State report |
| `device_info` | `{"fw_version":"2.0.0","hw_id":"Watcher-XXXX"}` | On-connect report |
| `fw_ota_progress` | `{"percent":45}` | OTA progress |
| `fw_ota_complete` | `{"status":"ok"}` | OTA done |
| `camera_ai_result` | `{"boxes":[...],"timestamp":...}` | AI detection |

### Binary Frames

| Direction | Format | Description |
|-----------|--------|-------------|
| Uplink | Raw PCM 16kHz 16-bit mono | Voice audio |
| Downlink | Raw PCM 24kHz 16-bit mono | TTS audio |
| Uplink | `WVID` frame (see above) | Video frame |

---

## Dependency Version Policy

### SenseCAP Watcher SDK — Version Lock

The `sensecap-watcher` SDK uses button component v3.x API (`BUTTON_TYPE_CUSTOM` with custom callbacks), which is incompatible with button v4.x. Upgrading would require rewriting SDK internals.

**Locked Versions:**

| Component | Version | Notes |
|-----------|---------|-------|
| `espressif/button` | `~3.2.3` | NOT v4.x — `BUTTON_TYPE_CUSTOM` removed |
| `espressif/esp_lvgl_port` | `~1.4.0` | NOT v2.x — requires button v4 |
| `lvgl/lvgl` | `~8.4.0` | Compatible with esp_lvgl_port v1.x |

**Rationale:**

1. **button v4 Breaking Changes:**
   - Removed `BUTTON_TYPE_CUSTOM` enum
   - New `button_driver_t` abstraction requires driver struct implementation
   - Callback signature changed: `void (*)(void *, void *)` → `void (*)(void *handle, void *usr_data)`

2. **sensecap-watcher SDK Constraints:**
   - Third-party SDK — should not modify internal implementation
   - Uses `BUTTON_TYPE_CUSTOM` with `button_custom_config_t` for rotary encoder button
   - Bundled with `esp_lvgl_port` v1.x compatibility

3. **Migration Risk:**
   - Requires forking/patching sensecap-watcher SDK
   - Future SDK updates would need re-patching
   - No benefit outweighs maintenance burden

**Decision:** Keep existing versions. Do NOT upgrade button or esp_lvgl_port.
