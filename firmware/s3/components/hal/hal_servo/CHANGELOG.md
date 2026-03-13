# Changelog

All notable changes to the hal_servo component will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.0.0] - 2025-03-13

### Added

- **LEDC PWM Direct Drive**: Full implementation replacing UART-to-MCU bridge
  - 50Hz frequency, 14-bit resolution
  - GPIO 19 (X-axis) and GPIO 20 (Y-axis)
  - Duty cycle: 819 (0°) to 1638 (180°)

- **Smooth Movement**: Background FreeRTOS task with linear interpolation
  - Configurable step interval (default 10ms)
  - Command queue for non-blocking operation

- **Synchronized Motion**: `hal_servo_move_sync()` for coordinated dual-axis movement

- **Thread Safety**: Mutex-protected angle state for multi-threaded access

- **Mechanical Protection**: Automatic Y-axis clamping to configurable limits (90-150°)

- **Documentation**: README.md with full API reference and usage examples

### Changed

- **Breaking**: Replaced stub implementation with full LEDC PWM control
- **Breaking**: `hal_servo_init()` now creates FreeRTOS task and queue

### Removed

- Phase 1 stub code (ESP_OK returns without hardware control)

## [1.0.0] - 2025-01-XX

### Added

- Initial stub implementation for Phase 1
- Basic API definitions in header file
- Kconfig options for GPIO and limits
- Stub functions returning ESP_OK to allow boot without hardware
