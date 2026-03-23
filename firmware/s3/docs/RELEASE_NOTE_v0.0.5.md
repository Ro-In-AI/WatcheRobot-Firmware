# WatcheRobot S3 Release Note v0.0.5

Date: 2026-03-23

Tag: `v0.0.5`

Branch: `main`

## Summary

`v0.0.5` merges the PNG animation system upgrade into `main` and stabilizes the display startup path on the S3 firmware.

This release turns the previous scattered PNG loading flow into a manifest-driven animation pipeline with warm first-frame previews, asynchronous hot-cache switching, and full-screen 2x display scaling for `206x206` assets on the `412x412` LCD.

## Highlights

- Added generated animation asset metadata: `anim_manifest.bin`
- Added generated first-frame `raw565` previews for fast warm-cache startup
- Switched runtime animation loading to lazy, per-type hot-cache construction
- Added async worker-based animation preparation with generation guards for arbitrary switching
- Added front/back image layer switching with lightweight fade transition
- Reduced LVGL draw buffer pressure to avoid display flush `ESP_ERR_NO_MEM` failures
- Restored stable WebSocket startup after display initialization
- Added 2x display zoom so `206x206` animation assets fill the `412x412` screen

## Animation System Settings

- Default animation FPS: `30`
- Supported configurable FPS range: `1..60`
- Maximum frames per animation type: `24`
- Warm cache budget: `1024 KB`
- Hot cache budget: `4096 KB`
- Safety margin: `512 KB`
- Switch fade duration: `140 ms`
- Lazy loading: enabled
- RGB565 cache: enabled

## Supported Animation Sets In This Release

- `boot`
- `greeting`
- `detecting`
- `detected`
- `speaking`
- `listening`
- `analyzing`
- `standby`
- `thinking`

## Display Behavior

- Source asset resolution: `206x206`
- LCD resolution: `412x412`
- Runtime display scaling: `2.0x`
- Anti-aliasing on scaled animation layers: disabled

## Validation Summary

- Firmware version updated to `0.0.5`
- Local `main` includes the animation system changes
- Build verification completed with `idf.py build`
- Device verification confirmed stable boot, stable WebSocket connection, and no recurring LVGL flush watchdog after the display pipeline adjustments
