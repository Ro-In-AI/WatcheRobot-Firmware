# Component Architecture Details

Extended architecture notes for WatcheRobot Firmware.

---

## HAL Layer

### hal_audio
- I2S DMIC recording at 16kHz
- I2S speaker playback at 24kHz
- Sample rate switching handled internally
- API: `hal_audio_read()`, `hal_audio_write()`

### hal_button
- GPIO button input handling
- Debounce and event callback interface
- Used for voice trigger (push-to-talk)

### hal_display
- SPD2010 QSPI LCD initialization
- LVGL port configuration
- Minimal init for boot animation: `hal_display_minimal_init()`
- Full UI init: `hal_display_ui_init()`

### hal_servo
- Direct LEDC PWM control (no secondary MCU)
- GPIO 19 → X-axis, GPIO 20 → Y-axis
- Smooth move via FreeRTOS task (10ms step)
- Synchronized dual-axis movement
- Y-axis mechanical limits via Kconfig

### hal_camera
- Wraps `sscma_client` component
- Himax HX6538 vision AI chip
- JPEG frame callback interface

---

## Services Layer

### voice_service
- State machine: Idle → Recording → Sent → Response
- Wake word detection via ESP-SR AFE
- Button-triggered recording
- Resumes wake word after TTS playback

### anim_service
- 30fps LVGL animation engine
- PNG frames decoded to RGB565 in PSRAM
- Lazy load strategy (only current animation in memory)
- Memory per frame: 332KB (412×412×2)

### camera_service
- Priority rules:
  - TTS playback → stream paused
  - Voice recording → throttled to 1fps
  - WS disconnected → stream stopped

### ota_service
- HTTP download + WebSocket trigger
- SHA256 validation
- Auto-rollback on crash

---

## Protocols Layer

### ws_client
- Connection management, auto-reconnect
- Binary frame handling (audio/video)
- Sub-modules: `ws_client.c`, `ws_router.c`, `ws_handlers.c`, `ws_protocol.c`

### discovery
- UDP broadcast for server discovery
- Device sends → server responds with IP:port → WebSocket connect

### ble_service
- GATT Control Service (UUID 0x1234)
- WiFi Provisioning over BLE
- WiFi + BLE coexistence enabled

---

## WebSocket Binary Formats

### WVID Video Frame
```
Offset  Size  Field
0       4     Magic: "WVID"
4       4     Timestamp (ms, uint32 BE)
8       2     Width
10      2     Height
12      4     JPEG size
16      N     JPEG data
```

### Audio
- Uplink: Raw PCM 16kHz 16-bit mono
- Downlink: Raw PCM 24kHz 16-bit mono
