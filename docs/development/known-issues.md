# Known Issues & Technical Notes

> Documented issues, workarounds, and technical decisions

---

## 1. AFE Ringbuffer Priority Sync Issue

### Status
**OPEN** — Partially mitigated, may still occur under load

### Description

ESP-SR AFE (Audio Front-End) ringbuffer has write/read timing sync issues regardless of `detection_task` priority:

| detection_task Priority | Symptom | Cause |
|------------------------|---------|-------|
| 4 (below voice_task) | Ringbuffer Full | voice_task writes too fast, detection_task can't fetch() |
| 5 (equal to voice_task) | Uncertain | Depends on task scheduling |
| 6 (above voice_task) | Ringbuffer Empty | detection_task reads too fast, voice_task can't feed() |

### Root Cause

Task execution rate mismatch:

1. **voice_recorder_task**: Executes every 60ms, reads one audio frame and feeds AFE
2. **detection_task**: Each fetch() consumes one frame, but execution timing is unpredictable

When detection_task priority > voice_task:
- detection_task immediately executes fetch()
- But voice_task hasn't executed feed() yet
- Result: ringbuffer empty

When detection_task priority < voice_task:
- voice_task continuously feeds data
- detection_task doesn't get enough CPU time to read
- Result: ringbuffer full

### Attempted Fixes

#### Fix 1: Adjust Priority (FAILED)

| Priority Config | Result |
|-----------------|--------|
| detection=4, voice=5 | Ringbuffer Full |
| detection=5, voice=5 | Uncertain |
| detection=6, voice=5 | Ringbuffer Empty |

#### Fix 2: Add Startup Delay (PARTIAL)

Added 100ms delay in `hal_wake_word_start()`:
- First startup can buffer a few frames
- Still has timing issues after stable operation

### Proposed Solutions

#### Option A: Semaphore Sync

```c
SemaphoreHandle_t feed_done_sem = xSemaphoreCreateBinary();

// In voice_recorder_task, after feed()
void hal_wake_word_feed(...) {
    // ... feed data
    xSemaphoreGive(feed_done_sem);
}

// In detection_task
void detection_task(void *arg) {
    while (1) {
        xSemaphoreTake(feed_done_sem, portMAX_DELAY);
        afe_fetch_result_t *res = ctx->afe_iface->fetch(ctx->afe_data);
        // ... process result
    }
}
```

**Drawback**: Per-frame semaphore adds overhead

#### Option B: Sync fetch() in feed()

Call fetch() immediately after feed() in same task:

```c
void hal_wake_word_feed(wake_word_ctx_t *ctx, const int16_t *samples, size_t num_samples) {
    // ... feed data

    // Immediately fetch to ensure data is consumed
    while (ctx->afe_iface->get_feed_chunksize(ctx->afe_data) <= ctx->input_buffer_size) {
        afe_fetch_result_t *res = ctx->afe_iface->fetch(ctx->afe_data);
        if (res && res->wakeup_state == WAKENET_DETECTED) {
            // Handle wake word detection
        }
    }
}
```

**Drawback**: May block voice_recorder_task, needs testing

#### Option C: Increase AFE Ringbuffer

Modify ESP-SR library to increase buffer size. May not be feasible without ESP support.

#### Option D: Lower voice_recorder_task Priority

Reduce from 5 to 4, giving detection_task (6) CPU time first.

**Status**: Not tested

#### Option E: Use Queue Instead

Create FreeRTOS queue, voice_recorder_task puts audio data in queue, detection_task retrieves:

```c
QueueHandle_t audio_queue = xQueueCreate(10, sizeof(audio_frame_t));

// voice_recorder_task
xQueueSend(audio_queue, &frame, 0);

// detection_task
if (xQueueReceive(audio_queue, &frame, 0)) {
    ctx->afe_iface->feed(ctx->afe_data, frame.samples);
    // Also fetch
}
```

**Drawback**: Adds complexity and latency

### Current Workaround

```c
// Current config in hal_wake_word.c
#define DETECTION_TASK_PRIO    6  // High priority
#define DETECTION_TASK_STACK   4096

// 100ms startup delay in hal_wake_word_start()
void hal_wake_word_start(wake_word_ctx_t *ctx) {
    xEventGroupSetBits(ctx->event_group, DETECTION_RUNNING_BIT);
    vTaskDelay(pdMS_TO_TICKS(100));  // Startup delay
    ESP_LOGI(TAG, "Wake word detection started");
}
```

### Related Files

- `components/services/voice_service/src/hal_wake_word.c`
- `components/services/voice_service/src/voice_fsm.c`
- `components/hal/hal_audio/src/hal_audio.c`

### References

- ESP-SR: https://github.com/espressif/esp-sr
- AFE API: `esp_afe_sr_iface_t`

---

## 2. LVGL PNG Decoder Memory

### Status
**RESOLVED** (v1.5.0)

### Problem

PNG images displayed as white/blank screen.

### Root Cause

1. `CONFIG_LV_USE_PNG` not enabled
2. Wrong image format: `LV_IMG_CF_TRUE_COLOR_ALPHA` instead of `LV_IMG_CF_RAW_ALPHA`
3. LVGL memory too small: 32KB insufficient for 412×412 RGBA (678KB)

### Solution

```c
// CORRECT - tells LVGL "this is PNG data, please decode"
img_dsc->header.cf = LV_IMG_CF_RAW_ALPHA;
```

Enable in sdkconfig:
- `CONFIG_LV_USE_PNG=y`
- `CONFIG_LV_MEM_CUSTOM=y` (use PSRAM)

### Lesson Learned

When using LVGL with PNG:
1. Enable `CONFIG_LV_USE_PNG`
2. Use `LV_IMG_CF_RAW_ALPHA` for PNG data
3. Use `LV_MEM_CUSTOM` with PSRAM for large images

---

## 3. SPI Transmit Conflict

### Status
**RESOLVED** (v1.4.0)

### Problem

LCD SPI transmit failed with:
```
E (10642) lcd_panel.io.spi: panel_io_spi_tx_color(390): spi transmit (queue) color failed
```

### Root Cause

WiFi DMA buffers consumed internal memory needed by LCD SPI.

### Solution

Sync sdkconfig with factory_firmware:
- `CONFIG_SPIRAM_SPEED_80M=y` (was 40MHz)
- `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`
- `CONFIG_ESP32S3_DATA_CACHE_SIZE_32KB=y`
- `CONFIG_SPI_MASTER_IN_IRAM=y`

---

## Changelog

| Date | Issue | Status |
|------|-------|--------|
| 2026-03-11 | AFE Ringbuffer Priority | OPEN |
| 2026-03-04 | LVGL PNG Memory | RESOLVED |
| 2026-02-28 | SPI Transmit Conflict | RESOLVED |
