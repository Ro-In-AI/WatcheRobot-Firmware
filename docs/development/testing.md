# Testing Guide

> Integration testing and quality assurance for WatcheRobot

---

## 1. Test Categories

| Category | Scope | Tools |
|----------|-------|-------|
| **Unit Tests** | Individual components | Unity (ESP-IDF built-in) |
| **Host Tests** | Pure logic modules | CMake + MinGW |
| **Integration Tests** | Multi-component flows | Manual + Automation |
| **E2E Tests** | Full system | Hardware-in-loop |

---

## 2. Unit Tests

### 2.1 Component Test Structure

```
my_component/
└── test_apps/
    └── main/
        ├── CMakeLists.txt
        └── test_my_component.c
```

### 2.2 Running Unit Tests

```bash
# In ESP-IDF environment
cd firmware/s3/components/hal/hal_servo/test_apps
idf.py -p COM3 flash monitor
```

### 2.3 Test Coverage

Each component should have tests for:
- Init/deinit lifecycle
- Public API functions
- Edge cases and error handling

---

## 3. Host Tests (PC)

For pure logic modules without hardware dependencies:

```bash
# Run host tests
cd firmware/s3/test_host
cmake -B build -G "MinGW Makefiles"
cmake --build build
ctest --test-dir build -V
```

---

## 4. Integration Tests

### 4.1 Connection Test

| # | Test | Expected | Status |
|---|------|----------|--------|
| 1.1 | ESP32-S3 WebSocket connect | Log: "WebSocket connected" | ⬜ |
| 1.2 | Display happy emoji | After connection success | ⬜ |
| 1.3 | UDP discovery | Server responds with IP:port | ⬜ |

### 4.2 Audio Flow Test

| # | Test | Expected | Status |
|---|------|----------|--------|
| 2.1 | Button long press → recording | Display "Listening..." | ⬜ |
| 2.2 | Send Raw PCM 16kHz | Server receives audio | ⬜ |
| 2.3 | Button release → audio_end | Server acknowledges | ⬜ |
| 2.4 | Receive asr_result | Display recognized text | ⬜ |
| 2.5 | Receive bot_reply | Display AI response | ⬜ |
| 2.6 | Receive TTS audio | Play Raw PCM 24kHz | ⬜ |
| 2.7 | tts_end received | Resume wake word | ⬜ |

### 4.3 Wake Word Test

| # | Test | Expected | Status |
|---|------|----------|--------|
| 3.1 | Say "Hi 乐鑫" | Wake word detected | ⬜ |
| 3.2 | Auto-start recording | VAD silence detection | ⬜ |
| 3.3 | Continuous conversation | Works after TTS end | ⬜ |

### 4.4 Servo Control Test

| # | Test | Expected | Status |
|---|------|----------|--------|
| 4.1 | servo X:90 | GPIO 19 PWM output | ⬜ |
| 4.2 | servo Y:120 | GPIO 20 PWM output | ⬜ |
| 4.3 | Smooth move | 500ms transition | ⬜ |
| 4.4 | Y-axis limit | 90–150° range enforced | ⬜ |

### 4.5 Display Test

| # | Test | Expected | Status |
|---|------|----------|--------|
| 5.1 | Animation playback | 30fps smooth | ⬜ |
| 5.2 | Emoji switch | < 100ms transition | ⬜ |
| 5.3 | Text display | Correct rendering | ⬜ |

---

## 5. Protocol Alignment

### 5.1 Audio Stream Protocol ✅ Aligned

| Feature | Server | Device | Status |
|---------|--------|--------|--------|
| Audio upload | Raw PCM | Raw PCM | ✅ |
| Recording end | `{"type":"audio_end"}` | Same | ✅ |
| ASR result | `{"type":"asr_result",...}` | Same | ✅ |
| TTS audio | Raw PCM 24kHz | Same | ✅ |
| Error | `{"type":"error",...}` | Same | ✅ |

### 5.2 Servo Protocol ✅ Aligned

| Feature | Server | Device | Status |
|---------|--------|--------|--------|
| Servo move | `{"type":"servo","data":{...}}` | GPIO PWM | ✅ |
| X-axis | 0–180° | GPIO 19 | ✅ |
| Y-axis | 90–150° | GPIO 20 | ✅ |

---

## 6. Test Environment

| Item | Value |
|------|-------|
| Server IP | `192.168.31.10` (or via discovery) |
| WebSocket Port | 8766 |
| UDP Discovery Port | 8767 |
| ESP32-S3 Port | COM3 (Windows) / /dev/ttyUSB0 (Linux) |

### Server Configuration

```bash
# watcher-server/.env
WS_PORT=8766
UDP_PORT=8767
```

---

## 7. Regression Checklist

Before each release:

- [ ] All unit tests pass
- [ ] Integration tests pass
- [ ] Wake word detection works
- [ ] Button recording works
- [ ] TTS playback works
- [ ] Servo control works
- [ ] Animation smooth at 30fps
- [ ] OTA update tested
- [ ] Memory stable (no leaks after 1hr)

---

## 8. Performance Benchmarks

| Metric | Target | Actual |
|--------|--------|--------|
| Wake word latency | < 500ms | ⬜ |
| ASR → LLM → TTS | < 3s | ⬜ |
| Animation FPS | 30 | ⬜ |
| Servo smooth move | 10ms step | ⬜ |
| PSRAM usage (anim) | < 6MB | ⬜ |
| Boot time | < 3s | ⬜ |

---

*Last updated: 2026-03-11*
