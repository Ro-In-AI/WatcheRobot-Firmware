# Development Roadmap

> WatcheRobot Firmware development phases and priorities

---

## Current Version: 2.0.0 (Phase 1 Complete)

Phase 1 delivered the four-layer component architecture migration from MVP-W prototype.

---

## Phase Overview

| Phase | Description | Priority | Status |
|-------|-------------|----------|--------|
| **Phase 1** | Architecture Migration | P0 | ✅ Complete |
| **Phase 2** | Servo Direct Drive | P0 | 🔲 Pending |
| **Phase 3** | Animation 30fps | P0 | 🔲 Pending |
| **Phase 4** | Dual OTA Partition | P1 | 🔲 Pending |
| **Phase 5** | Firmware OTA | P1 | 🔲 Pending |
| **Phase 6** | BLE Service | P1 | 🔲 Pending |
| **Phase 7** | Camera Streaming | P1 | 🔲 Pending |
| **Phase 8** | Animation OTA | P2 | 🔲 Pending |

---

## Phase 1: Architecture Migration ✅

**Status**: Complete
**Commit**: `cf1900f`

### Deliverables
- [x] Four-layer component structure (drivers/hal/protocols/services/utils)
- [x] Code migration from MVP-W `main/` to components
- [x] Remove `cJSON.c` → use ESP-IDF `json` component
- [x] Remove `uart_bridge` → will be replaced by `hal_servo`
- [x] Component `CMakeLists.txt` with proper `REQUIRES`/`PRIV_REQUIRES`
- [x] `idf_component.yml` manifests
- [x] Documentation structure

---

## Phase 2: Servo Direct Drive (P0)

**Status**: Pending
**Prerequisite**: Phase 1
**Est. Effort**: 2-3 days

### Goal
Replace UART→MCU servo control with direct GPIO 19/20 LEDC PWM.

### Component: `hal/hal_servo`

```
hal_servo/
├── CMakeLists.txt
├── idf_component.yml
├── Kconfig
├── README.md
├── include/
│   └── hal_servo.h
├── src/
│   └── hal_servo.c
└── test_apps/
    └── main/
        └── test_hal_servo.c
```

### Key Features
- LEDC Timer 0, 50Hz, 14-bit resolution
- Channel 0 → GPIO 19 (X-axis)
- Channel 1 → GPIO 20 (Y-axis)
- Smooth move with FreeRTOS background task (10ms step)
- Y-axis mechanical limit: 90°–150° (Kconfig)

### API
```c
esp_err_t hal_servo_init(void);
esp_err_t hal_servo_set_angle(servo_axis_t axis, int angle_deg);
esp_err_t hal_servo_move_smooth(servo_axis_t axis, int angle_deg, int duration_ms);
esp_err_t hal_servo_move_sync(int x_deg, int y_deg, int duration_ms);
int hal_servo_get_angle(servo_axis_t axis);
```

### Migration Source
`firmware/mcu/main/servo_control.c` → Logic移植, GPIO changed

---

## Phase 3: Animation 30fps (P0)

**Status**: Pending
**Prerequisite**: Phase 1
**Est. Effort**: 3-4 days

### Goal
Achieve smooth 30fps animation playback with PNG→RGB565 PSRAM caching.

### Component: `services/anim_service`

### Key Changes
- **anim_storage.c**: Load PNG, decode to RGB565, store in PSRAM
- **anim_player.c**: Use `lv_animimg` widget, < 1ms frame switch
- **anim_meta.c**: Parse `/spiffs/anim/anim_meta.json`

### Memory Strategy
- Single frame: 412 × 412 × 2 = 332KB (RGB565)
- Lazy load: Only current animation type's frames in PSRAM
- Max ~18 frames per animation type

### Target Performance
- FPS: 30 (was ~6.7fps)
- Frame switch: < 1ms
- Type switch: < 500ms (decode + load)

---

## Phase 4: Dual OTA Partition (P1)

**Status**: Pending
**Prerequisite**: Phase 1
**Est. Effort**: 1 day

### Goal
Switch from `factory` to `ota_0`/`ota_1` partition layout.

### Partition Table
```csv
# Name,    Type, SubType,  Offset,    Size
nvs,       data, nvs,      0x9000,    0x6000
phy_init,  data, phy,      0xF000,    0x1000
otadata,   data, ota,      0x10000,   0x2000
ota_0,     app,  ota_0,    0x20000,   0x500000
ota_1,     app,  ota_1,    0x520000,  0x500000
model,     data, spiffs,   0xA20000,  0x50000
storage,   data, spiffs,   0xA70000,  0x580000
```

### Notes
- First flash requires full erase
- Manual `esp_ota_set_boot_partition()` for initial setup

---

## Phase 5: Firmware OTA (P1)

**Status**: Pending
**Prerequisite**: Phase 4
**Est. Effort**: 2-3 days

### Component: `services/ota_service`

### Flow
1. Server sends `fw_ota_notify` with URL + SHA256
2. Device downloads via `esp_https_ota`
3. Validate SHA256, set boot partition
4. Reboot
5. `ota_service_mark_valid()` on success (prevent rollback)

### API
```c
esp_err_t ota_service_init(void);
esp_err_t ota_service_start(const char *url, const char *expected_version);
void ota_service_mark_valid(void);
const char* ota_service_get_fw_version(void);
```

---

## Phase 6: BLE Service (P1)

**Status**: Pending
**Prerequisite**: Phase 1
**Est. Effort**: 3-4 days

### Component: `protocols/ble_service`

### Features
1. **GATT Control Service** (UUID 0x1234)
   - Servo control
   - Display update
   - Status notification

2. **WiFi Provisioning**
   - ESP-IDF `wifi_provisioning` over BLE

### Config
```kconfig
config WATCHER_BLE_ENABLE
    bool "Enable BLE service"
    default n
```

### Coexistence
- Enable `CONFIG_ESP32_WIFI_SW_COEXIST_ENABLE`
- Limit BLE advertising during TTS playback

---

## Phase 7: Camera Streaming (P1)

**Status**: Pending
**Prerequisite**: Phase 1 + Hardware verification
**Est. Effort**: 4-5 days

### Components
- `hal/hal_camera`: SSCMA client wrapper
- `services/camera_service`: Streaming orchestration

### Priority Rules
- TTS playing → Stream paused
- Voice recording → Stream throttled to 1fps
- WebSocket disconnected → Stream stopped

### Binary Frame Format (WVID)
```
Offset  Size  Field
0       4     Magic: "WVID"
4       4     Timestamp (ms)
8       2     Width
10      2     Height
12      4     JPEG size
16      N     JPEG data
```

---

## Phase 8: Animation OTA (P2)

**Status**: Pending
**Prerequisite**: Phase 3
**Est. Effort**: 2-3 days

### Goal
Hot-swap animation assets via WebSocket.

### Flow
1. Server sends `anim_ota_start` with metadata
2. Device receives PNG frames via binary WebSocket
3. Write to `/spiffs/anim/` SPIFFS
4. Update `anim_meta.json`
5. Reload animation

---

## Dependencies Graph

```
Phase 1 (Architecture)
    │
    ├──► Phase 2 (Servo)
    │
    ├──► Phase 3 (Animation)
    │        │
    │        └──► Phase 8 (Anim OTA)
    │
    ├──► Phase 4 (Partitions)
    │        │
    │        └──► Phase 5 (Firmware OTA)
    │
    ├──► Phase 6 (BLE)
    │
    └──► Phase 7 (Camera)
```

---

## Milestone Releases

| Version | Phases | Target |
|---------|--------|--------|
| 2.1.0 | Phase 2 + 3 | Core functionality |
| 2.2.0 | Phase 4 + 5 | OTA ready |
| 2.3.0 | Phase 6 + 7 | Full featured |
| 3.0.0 | Phase 8 + Refinements | Production ready |

---

*Last updated: 2026-03-11*
