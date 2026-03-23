# Animation Service

High-performance emoji animation playback service for WatcheRobot firmware.

## Features

- **30fps Playback**: Smooth animation with configurable frame rate (1-60 fps)
- **RGB565 Cache**: Decoded frames cached in PSRAM for <1ms frame switching
- **Lazy Loading**: Only current animation type loaded in memory
- **Type Switching**: <500ms latency when switching animation types
- **Metadata System**: Runtime configuration via JSON file

## Architecture

```
┌─────────────────┐     ┌──────────────────┐     ┌─────────────────┐
│   anim_player   │────▶│   anim_storage   │────▶│    SPIFFS       │
│  (Timer-based   │     │  (RGB565 Cache)  │     │  (PNG Files)    │
│   playback)     │     └──────────────────┘     └─────────────────┘
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│   anim_meta     │
│  (JSON Config)  │
└─────────────────┘
```

## API

### Initialization

```c
#include "anim_player.h"

// After LVGL and SPIFFS are initialized
lv_obj_t *img_obj = lv_img_create(lv_scr_act());
emoji_anim_init(img_obj);
```

### Playing Animations

```c
// Start animation (auto-caches RGB565 frames)
emoji_anim_start(EMOJI_ANIM_SPEAKING);

// Stop animation
emoji_anim_stop();

// Show static frame
emoji_anim_show_static(EMOJI_ANIM_STANDBY, 0);

// Prefetch for reduced latency
emoji_anim_prefetch_type(EMOJI_ANIM_LISTENING);
```

### Animation Types

| Type | Description | File Prefix |
|------|-------------|-------------|
| `EMOJI_ANIM_HAPPY` | Positive / ready feedback | `happy*.png` |
| `EMOJI_ANIM_ERROR` | Error / failure feedback | `error*.png` |
| `EMOJI_ANIM_SPEAKING` | TTS playback | `speaking*.png` |
| `EMOJI_ANIM_LISTENING` | Voice recording | `listening*.png` |
| `EMOJI_ANIM_PROCESSING` | AI/task processing | `processing*.png` |
| `EMOJI_ANIM_STANDBY` | Idle state | `standby*.png` |
| `EMOJI_ANIM_THINKING` | Transitional thinking state | `thinking*.png` |
| `EMOJI_ANIM_CUSTOM_1` | Reserved custom state | `custom1*.png` |
| `EMOJI_ANIM_CUSTOM_2` | Reserved custom state | `custom2*.png` |
| `EMOJI_ANIM_CUSTOM_3` | Reserved custom state | `custom3*.png` |

## Configuration

### Kconfig Options

| Option | Default | Description |
|--------|---------|-------------|
| `WATCHER_ANIM_FPS` | 30 | Target frame rate |
| `WATCHER_ANIM_MAX_FRAMES_PER_TYPE` | 18 | Max frames per type |
| `WATCHER_ANIM_LAZY_LOAD` | y | Only cache current type |
| `WATCHER_ANIM_CACHE_RGB565` | y | Enable RGB565 cache |
| `WATCHER_ANIM_DEBUG_PERF` | n | Log performance metrics |

### Metadata File (Optional)

Place `anim_meta.json` in `/spiffs/anim/`:

```json
{
  "version": "1.0",
  "default_fps": 30,
  "animations": {
    "speaking": {
      "fps": 24,
      "loop": true
    },
    "standby": {
      "fps": 15,
      "loop": true
    }
  }
}
```

## Memory Usage

| Component | Size |
|-----------|------|
| Frame (412x412 RGB565) | 332 KB |
| 18 frames (max) | ~6 MB |
| PNG source (typical) | 30-50 KB each |

## Performance

| Metric | Target | Actual |
|--------|--------|--------|
| Frame rate | 30 fps | 30 fps |
| Frame switch latency | <1 ms | ~0.5 ms |
| Type switch latency | <500 ms | ~200-400 ms |

## Dependencies

- `lvgl` (v8.4.x)
- `json` (ESP-IDF component)
- `spiffs`
- `sensecap-watcher`
- `esp_lvgl_port`
- `hal_display`
