# Voice Stream Protocol v2.0

> Audio streaming specification for WatcheRobot
> Version: 2.0 | Date: 2026-03-11

See also: [WebSocket Protocol](websocket-protocol.md) for full protocol details.

---

## 1. Audio Format

| Parameter | Recording (ASR) | Playback (TTS) |
|-----------|-----------------|----------------|
| Format | PCM (Raw Binary) | PCM (Raw Binary) |
| Sample Rate | 16000 Hz | 24000 Hz |
| Bit Depth | 16-bit signed | 16-bit signed |
| Channels | Mono | Mono |
| Endianness | Little-endian | Little-endian |
| Frame Size | 1920 bytes (60ms) | Variable |

**Bandwidth**:
- Upload: 256 kbps
- Download: 384 kbps

---

## 2. Device → Server

### 2.1 Audio Data (Binary)

```
WebSocket Binary Frame
┌─────────────────────────┐
│   Raw PCM Data (N B)    │
│   16kHz, 16-bit, mono  │
└─────────────────────────┘
```

No header — raw PCM samples only.

### 2.2 Recording End (JSON)

```json
{"type": "audio_end"}
```

### 2.3 Status Report

```json
{"type": "status", "state": "listening" | "recording" | "thinking" | "speaking" | "idle" | "error"}
```

---

## 3. Server → Device

### 3.1 ASR Result

```json
{"type": "asr_result", "code": 0, "data": "recognized text"}
```

### 3.2 Bot Reply

```json
{"type": "bot_reply", "code": 0, "data": "AI response"}
```

### 3.3 TTS Start

```json
{"type": "status", "state": "speaking"}
```

### 3.4 TTS Audio (Binary)

```
WebSocket Binary Frame
┌─────────────────────────┐
│   Raw PCM Data (N B)    │
│   24kHz, 16-bit, mono  │
└─────────────────────────┘
```

### 3.5 TTS End

```json
{"type": "tts_end", "code": 0, "data": "ok"}
```

### 3.6 Error

```json
{"type": "error", "code": 1, "data": "error description"}
```

---

## 4. Complete Flow

```
[S3 Device]                              [Cloud Server]
     │                                        │
     │  1. Wake word detected / Button press  │
     │                                        │
     │  2. Raw PCM chunks (16kHz)            │
     │  ─────────────────────►                │
     │                                        │
     │  3. {"type": "audio_end"}              │
     │  ─────────────────────►                │
     │                                        │
     │                    4. ASR recognition  │
     │  ◄─────────────────────                │
     │     {"type": "asr_result", ...}        │
     │                                        │
     │                    5. LLM processing   │
     │                                        │
     │  6. {"type": "bot_reply", ...}         │
     │  ◄─────────────────────                │
     │                                        │
     │  7. {"type": "status", "speaking"}     │
     │  ◄─────────────────────                │
     │                                        │
     │  8. TTS audio stream (24kHz)           │
     │  ◄─────────────────────                │
     │     Raw PCM chunks                     │
     │                                        │
     │  9. {"type": "tts_end"}                │
     │  ◄─────────────────────                │
     │                                        │
     │  10. Resume wake word detection        │
     │                                        │
     ▼                                        ▼
```

---

## 5. Device Implementation

### 5.1 Sending Audio

```c
// components/protocols/ws_client/src/ws_client.c

// Send PCM data
ws_send_audio(pcm_data, len);

// Send end marker
ws_send_audio_end();

// Send status report
ws_send_status("recording");
```

### 5.2 Receiving Handler

```c
// Handle JSON text messages
void ws_handle_text(const char *msg) {
    cJSON *root = cJSON_Parse(msg);
    const char *type = cJSON_GetObjectItem(root, "type")->valuestring;

    if (strcmp(type, "asr_result") == 0) {
        // Display recognition result
    }
    else if (strcmp(type, "tts_end") == 0) {
        // TTS playback complete, resume wake word
        voice_recorder_resume_wake_word();
    }
    else if (strcmp(type, "status") == 0) {
        // State update
    }
}

// Handle binary TTS audio
void ws_handle_tts_binary(const uint8_t *data, int len) {
    hal_audio_write(data, len);  // Direct write Raw PCM 24kHz
}
```

---

## 6. Wake Word Mode

### 6.1 Detection Flow

1. `hal_wake_word_init()` — Initialize AFE model
2. `voice_recorder_task` — Poll and feed PCM data
3. Wake word "Hi 乐鑫" detected
4. Trigger callback `on_wake_word_detected()`
5. Start recording, enable VAD silence detection

### 6.2 VAD Silence Detection

- Silence timeout: 1500ms (default)
- RMS threshold: 100
- Minimum speech: 300ms

---

## 7. Service Discovery

- UDP broadcast port: 8767
- WebSocket port: 8766

---

*Version 2.0 — Compatible with watcher-server v2.0*
*See COMMUNICATION_PROTOCOL.md for complete protocol details*
