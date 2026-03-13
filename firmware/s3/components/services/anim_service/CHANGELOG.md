# Changelog

All notable changes to the Animation Service component will be documented in this file.

## [2.1.0] - 2024-03-13

### Added
- RGB565 cache system for fast frame switching (<1ms latency)
- Metadata system with JSON configuration support
- `anim_meta.h/c` - Runtime animation configuration
- `anim_cache_*` API for PSRAM-based frame caching
- `emoji_anim_prefetch_type()` for proactive cache loading
- `emoji_anim_get_fps()` / `emoji_anim_set_fps()` runtime FPS control
- Kconfig options for animation parameters
- Performance logging (configurable via `WATCHER_ANIM_DEBUG_PERF`)

### Changed
- Default frame rate increased from ~6.7fps (150ms interval) to 30fps (33ms interval)
- Maximum frames per type increased from 10 to 18
- Animation now uses RGB565 cache by default for smooth playback
- Timer reuses existing handle instead of recreating on each start

### Fixed
- Frame switching latency reduced from ~150ms (PNG decode) to <1ms (cached RGB565)

## [2.0.0] - 2024-02-XX

### Added
- Initial release with PNG-based animation playback
- Support for 7 animation types
- LVGL timer-based frame cycling
- SPIFFS-based image loading

### Notes
- Frame rate limited to ~6.7fps due to PNG decode overhead
