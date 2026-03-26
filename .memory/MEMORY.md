# WatcheRobot Firmware Memory

Persistent context for Codex / Claude Code sessions.

---

## Project Identity

- **Name**: WatcheRobot Firmware
- **Target**: ESP32-S3 (SenseCAP Watcher hardware)
- **Framework**: ESP-IDF v5.2.1
- **Language**: C (embedded)

---

## Architecture Summary

Four-layer component model with strict dependency rules:

```
services/  ->  protocols/  ->  hal/  ->  drivers/
    |              |           |         |
   utils/        utils/      utils/   ESP-IDF
```

**Key Components**:
- Drivers: `bsp_watcher` (sensecap-watcher SDK wrapper)
- HAL: `hal_audio`, `hal_display`, `hal_servo`, `hal_camera`, `hal_button`
- Services: `voice_service`, `anim_service`, `camera_service`, `ota_service`, `behavior_state_service`, `sfx_service`
- Protocols: `ws_client`, `discovery`, `ble_service`
- Utils: `wifi_manager`, `boot_anim`

---

## Build Workflow

> Use PowerShell / ESP-IDF terminal / repo wrapper. Avoid Git Bash/MSys directly running `idf.py`.

1. Enter `firmware/s3`
2. Standard build:
   - `cmd /c "\"C:\\Espressif\\frameworks\\esp-idf-v5.2.1\\export.bat\" && idf.py build"`
3. Clean rebuild:
   - `cmd /c "\"C:\\Espressif\\frameworks\\esp-idf-v5.2.1\\export.bat\" && idf.py fullclean build"`
4. Standard flash+monitor:
   - `flash-monitor.cmd`
   - or `flash-monitor.cmd COM24`
5. If flashing fails with `Failed to connect to ESP32-S3: No serial data received`, the board likely did not enter bootloader. Try manual `BOOT + RESET`.

---

## Hardware Notes

- Servo X: GPIO 19 (0-180 deg)
- Servo Y: GPIO 20 (90-150 deg mechanical limit)
- Display: SPD2010 QSPI 412x412
- Camera: Himax HX6538 (SSCMA protocol)
- Audio: I2S DMIC + I2S speaker

---

## Code Patterns

- Functions: `{component}_{verb}_{noun}`
- Types: `{component}_{name}_t`
- Most public APIs return `int` or `esp_err_t` depending on component style
- Prefer Kconfig or protocol fields over new hardcoded constants

---

## Dependency Version Policy

**SenseCAP Watcher SDK 依赖锁定，不升级**

SenseCAP Watcher SDK 使用 button v3.x API (`BUTTON_TYPE_CUSTOM`)，与 button v4.x 不兼容。

**锁定版本:**
- `espressif/button`: `~3.2.3`
- `espressif/esp_lvgl_port`: `~1.4.0`
- `lvgl/lvgl`: `~8.4.0`

**原因:**
- button v4 移除了 `BUTTON_TYPE_CUSTOM`
- esp_lvgl_port v2.x 依赖 button v4.x
- sensecap-watcher 是第三方 SDK，不应大改其内部实现

---

## Protocol Snapshot

- **Source of truth**: `D:\GithubRep\watcher-server\docs\device_communication_protocol.md`
- **Current protocol version**: `0.1.5`
- **Discovery**: `ANNOUNCE` 必须包含 `ip / port / version / protocol_version / server`
- **WS handshake**:
  - hardware first sends `sys.client.hello`
  - server returns `sys.ack(type=sys.client.hello)`
  - firmware then enters `session_ready`
- **Binary framing**: unified `WSPK` `14B` header
  - `magic(4) + frame_type(1) + flags(1) + seq(4) + payload_len(4)`
  - `audio=1`, `video=2`, `image=3`, `ota=4`
- **AI status resources**: `evt.ai.status` currently supports `status / message / image_name / action_file / sound_file`
- **Firmware compatibility extensions**:
  - `ctrl.robot.state.set`
  - `evt.camera.state`

---

## Current Integration Status

- Servo 协议已经按 watcher `v0.1.5` 打通
- Discovery / WS version gating 已接入，代码常量当前为 `0.1.5`
- 音频上行与 TTS 下行已经切到 `WSPK` 音频帧
- 图片 / 视频上行已经切到 `WSPK` 图片视频帧
- OTA 当前只有协议入口与 `nack/not_supported` 占位，不做真实升级
- `evt.ai.status` 资源字段已经接入本地 behavior state 解析

---

## Current Runtime Notes

- Display/LVGL 最近做过一次稳定性收敛：
  - LCD transfer queue depth 收敛到 `1`
  - `CONFIG_BSP_LCD_SPI_DMA_SIZE_DIV=64`
  - `CONFIG_LVGL_DRAW_BUFF_HEIGHT=8`
  - 目的是缓解 `panel_io_spi_tx_color ... ESP_ERR_NO_MEM`
- Camera / voice 联调阶段，按键录音之外还要关注动画切换和显示刷新压力
- 当前刷机仍可能失败在 `Failed to connect to ESP32-S3: No serial data received`
  - `COM24` 可见时，也可能只是没被自动拉进 bootloader

---

## Session History

| Date | Summary |
|------|---------|
| 2026-03-26 | 对照 watcher-server `device_communication_protocol.md v0.1.5` 校准本地冻结文档；确认仓库 memory 原先停留在 2026-03-13，已补齐当前协议与联调状态 |
| 2026-03-24 | 固件协议版本提升到 `0.1.5`；`evt.ai.status` 开始支持 `image_name / action_file / sound_file` 资源字段 |
| 2026-03-24 | 相机/语音联调中定位显示刷新 `ESP_ERR_NO_MEM`，收敛 LCD DMA 与 LVGL draw buffer 配置 |
| 2026-03-23 | 舵机控制协议与 watcher-server 新版 WS 协议完成首轮打通 |
| 2026-03-12 | 决定锁定 sensecap-watcher 依赖版本，不升级 button/LVGL |
| 2026-03-11 | Session resume, documentation review, memory files updated |
| 2026-03-11 | Phase 1 component architecture migration completed |
