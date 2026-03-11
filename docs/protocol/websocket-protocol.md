# WebSocket Protocol v2.3

> WatcheRobot Firmware communication protocol
> Version: 2.3 | Date: 2026-03-11

---

## 1. System Architecture

```
┌─────────────────────────────┐      WebSocket (WiFi)      ┌─────────────────────────────┐
│      WatcheRobot (S3)       │◄──────────────────────────►│        Cloud Server         │
│         ESP32-S3            │     ws://IP:8766           │       Python Server         │
├─────────────────────────────┤                            ├─────────────────────────────┤
│ • Audio capture/playback    │                            │ • ASR (Aliyun)              │
│ • LVGL display + animation  │                            │ • LLM (Claude API)          │
│ • Wake word detection       │                            │ • TTS (Volcengine)          │
│ • Servo control (GPIO PWM)  │                            │ • Agent orchestration       │
│ • Camera streaming          │                            │ • OTA management            │
└─────────────────────────────┘                            └─────────────────────────────┘
```

**Key Changes from v2.x**:
- MCU removed — servo control via GPIO 19/20 LEDC PWM (no UART)
- Camera streaming support added
- OTA firmware update support added

---

## 2. Message Format

### 2.1 JSON Text Messages

All JSON messages use unified format:

```json
{
  "type": "message_type",
  "code": 0,
  "data": "payload"
}
```

| Field | Type | Description |
|-------|------|-------------|
| type | string | Message type identifier |
| code | int | Status code: `0` = success, `1` = error |
| data | any | Payload (string, object, array, etc.) |

### 2.2 Binary Messages

| Direction | Format | Description |
|-----------|--------|-------------|
| Device → Server | Raw PCM 16kHz 16-bit mono | Voice audio |
| Server → Device | Raw PCM 24kHz 16-bit mono | TTS audio |
| Device → Server | WVID frame (see §5) | Camera video |

---

## 3. Server → Device Messages

### 3.1 ASR Result (`asr_result`)

Speech recognition result text.

```json
{"type": "asr_result", "code": 0, "data": "recognized text"}
```

**Device handling**: Display text, switch to `analyzing` animation.

---

### 3.2 Bot Reply (`bot_reply`)

AI response text.

```json
{"type": "bot_reply", "code": 0, "data": "AI response content"}
```

**Device handling**: Optional text display, prepare for TTS audio.

---

### 3.3 Status (`status`)

System status notification.

```json
{"type": "status", "code": 0, "data": "status description"}
```

Examples:
- `[thinking] Processing...` — AI thinking
- `Servo animation started: speech_nod`

---

### 3.4 Error (`error`)

Error notification.

```json
{"type": "error", "code": 1, "data": "error description"}
```

**Device handling**: Display error state, switch to `sad` animation.

---

### 3.5 Servo Control (`servo`) — Updated v2.3

Real-time servo angle command. **Now controls GPIO directly** (no UART).

```json
{"type": "servo", "code": 0, "data": {"id": "X", "angle": 90, "time": 500}}
```

| Field | Type | Range | Description |
|-------|------|-------|-------------|
| id | string | "X" or "Y" | Servo axis identifier |
| angle | int | 0–180 | Target angle (degrees) |
| time | int | 0–5000 | Move duration (ms), optional, default 0 |

**Device handling**:
```
ws_router → on_servo_handler() → hal_servo_move_smooth(axis, angle, time)
                                       ↓
                               GPIO 19/20 LEDC PWM
```

**GPIO Mapping**:
- GPIO 19 → Servo X-axis (left/right, 0–180°)
- GPIO 20 → Servo Y-axis (up/down, 90–150° mechanical limit)

---

### 3.6 Display (`display`)

Control screen text and animation.

```json
{"type": "display", "code": 0, "data": {"text": "Hello!", "emoji": "happy"}}
```

| Field | Type | Description |
|-------|------|-------------|
| text | string | Display text (optional) |
| emoji | string | Animation name (optional) |

**Available animations**: `happy`, `sad`, `surprised`, `angry`, `standby`, `idle`, `listening`, `analyzing`, `thinking`, `speaking`

---

### 3.7 TTS End (`tts_end`)

TTS synthesis complete, all audio sent.

```json
{"type": "tts_end", "code": 0, "data": "ok"}
```

**Device handling**:
- Wait for I2S buffer to drain (~500ms)
- Stop audio playback
- Switch to `happy` animation
- Resume wake word detection

---

### 3.8 TTS Audio (Binary)

TTS synthesized audio data.

- **Format**: Raw PCM (no header)
- **Sample rate**: 24000 Hz
- **Bit depth**: 16-bit signed
- **Channels**: Mono

**Device handling**: Write directly to I2S speaker.

---

### 3.9 Camera Control (New v2.3)

#### Start streaming:
```json
{"type": "camera_start", "fps": 5}
```

#### Stop streaming:
```json
{"type": "camera_stop"}
```

#### Single capture:
```json
{"type": "camera_capture"}
```

---

### 3.10 OTA Notification (New v2.3)

Firmware update trigger.

```json
{
  "type": "fw_ota_notify",
  "version": "2.1.0",
  "url": "https://server/firmware/watcher-2.1.0.bin",
  "sha256": "abc123..."
}
```

**Device handling**:
1. Download firmware via HTTP (`esp_https_ota`)
2. Write to OTA partition
3. Validate SHA256
4. Set boot partition, reboot

---

### 3.11 Reboot (`reboot`)

Remote reboot command.

```json
{"type": "reboot"}
```

---

## 4. Device → Server Messages

### 4.1 Voice Audio (Binary)

PCM audio stream from microphone.

- **Format**: Raw PCM (no header)
- **Sample rate**: 16000 Hz
- **Bit depth**: 16-bit signed
- **Channels**: Mono
- **Frame size**: 1920 bytes (60ms)

---

### 4.2 Audio End (`audio_end`)

Recording complete marker.

```json
{"type": "audio_end"}
```

---

### 4.3 Status Report (`status`)

Device state report.

```json
{"type": "status", "state": "listening"}
```

**States**: `listening`, `recording`, `thinking`, `speaking`, `idle`, `error`

---

### 4.4 Device Info (`device_info`)

Sent on WebSocket connect.

```json
{
  "type": "device_info",
  "fw_version": "2.0.0",
  "hw_id": "Watcher-XXXX",
  "ota_partition": "ota_0"
}
```

---

### 4.5 OTA Progress (`fw_ota_progress`)

OTA download progress report.

```json
{"type": "fw_ota_progress", "percent": 45}
```

---

### 4.6 OTA Complete (`fw_ota_complete`)

OTA finished notification.

```json
{"type": "fw_ota_complete", "status": "ok"}
```

---

### 4.7 Camera AI Result (`camera_ai_result`)

Object detection result from Himax.

```json
{
  "type": "camera_ai_result",
  "boxes": [
    {"class": "person", "confidence": 0.95, "x": 100, "y": 80, "w": 50, "h": 120}
  ],
  "timestamp": 1234567890
}
```

---

## 5. Binary Frame Formats

### 5.1 Video Frame (WVID)

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

## 6. Service Discovery

UDP broadcast for server discovery.

**Port**: 8767

**Device broadcast**:
```json
{"type": "discovery", "device": "WatcheRobot", "firmware": "2.0.0"}
```

**Server response**:
```json
{"type": "discovery", "ip": "192.168.1.100", "port": 8766}
```

---

## 7. Message Flow Examples

### 7.1 Voice Conversation (Button Trigger)

```
Device                              Server
  │                                   │
  │  Button pressed → start recording │
  │  ─── Binary PCM 16kHz ───────────►│
  │  ─── {"type": "audio_end"} ──────►│
  │                                   │
  │  ◄── {"type": "asr_result"} ──────│
  │  ◄── {"type": "bot_reply"} ───────│
  │  ◄── Binary PCM 24kHz (TTS) ──────│
  │  ◄── {"type": "tts_end"} ─────────│
  │                                   │
  │  Resume wake word detection       │
  │                                   │
```

### 7.2 Wake Word Trigger

```
Device                              Server
  │                                   │
  │  Wake word "Hi 乐鑫" detected      │
  │  ─── {"type": "status",           │
  │       "state": "listening"} ─────►│
  │                                   │
  │  VAD detects speech → recording   │
  │  ─── Binary PCM 16kHz ───────────►│
  │  (VAD silence → auto stop)        │
  │  ─── {"type": "audio_end"} ──────►│
  │                                   │
  │  (Same as button flow)            │
  │                                   │
```

### 7.3 Servo Control Flow

```
Device                              Server
  │                                   │
  │  ◄── {"type": "servo",            │
  │       "data": {"id":"X",          │
  │       "angle": 90, "time": 500}} ─│
  │                                   │
  │  hal_servo_move_smooth(X,90,500)  │
  │  → GPIO 19 LEDC PWM               │
  │                                   │
```

---

## 8. Animation Mapping

| emoji | Animation | Use Case |
|-------|-----------|----------|
| `happy` | GREETING | Welcome, TTS complete |
| `sad` | DETECTED | Error, regret |
| `surprised` | DETECTING | Unexpected event |
| `angry` | ANALYZING | Warning |
| `standby` / `idle` | STANDBY | Default, wake word detection |
| `listening` | LISTENING | Recording, wake word detected |
| `analyzing` / `thinking` | ANALYZING | AI processing |
| `speaking` | LISTENING (temp) | TTS playback |

---

## 9. Audio Parameters

| Direction | Purpose | Sample Rate | Format | Bandwidth |
|-----------|---------|-------------|--------|-----------|
| Device → Server | Voice upload | 16kHz | 16-bit PCM mono | 256 kbps |
| Server → Device | TTS playback | 24kHz | 16-bit PCM mono | 384 kbps |

---

## Changelog

| Version | Date | Changes |
|---------|------|---------|
| 2.3 | 2026-03-11 | Servo GPIO direct control, camera streaming, OTA support |
| 2.2 | 2026-03-11 | Architecture restructure (MCU removed) |
| 2.1 | 2026-03-11 | Added display message, audio_end, wake word flow |
| 2.0 | 2026-03-01 | Unified message format, removed AUD1 header, added asr_result/bot_reply/tts_end |
| 1.1 | 2026-02-28 | Audio format changed from Opus to raw PCM |
| 1.0 | 2026-02-28 | Initial version |
