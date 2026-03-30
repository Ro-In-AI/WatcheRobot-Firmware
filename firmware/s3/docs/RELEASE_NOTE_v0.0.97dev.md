# WatcheRobot S3 Release Note v0.0.97dev

Date: 2026-03-30

Tag: `v0.0.97dev`

Branch: `codex/ble-refactor-plan-v0.0.97dev`

## Summary

`v0.0.97dev` is a planning milestone for the Bluetooth protocol refactor on the S3 firmware.

This commit does not expand the BLE feature surface yet. Its purpose is to freeze the next-step architecture, align versioning, and make the full refactor sequence explicit before the protocol is split into smaller modules.

## Current Baseline

- BLE is enabled with Bluedroid GATT server mode in `sdkconfig.defaults`
- The current implementation is concentrated in `components/protocols/ble_service/src/ble_service.c`
- Current scope remains motion control over GATT write with compatibility for the validated mobile app
- Wi-Fi provisioning is intentionally out of the current public BLE API surface

## Refactor Goals

- Keep backward compatibility for the existing BLE control app during the transition
- Reduce the size and responsibility concentration of `ble_service.c`
- Separate protocol parsing, transport, execution, and state reporting
- Make room for Wi-Fi provisioning and richer device status without destabilizing motion control
- Establish a versioned BLE protocol contract that can evolve independently from transport internals

## Planned Refactor Phases

### Phase 1: Protocol Freeze and Versioning

- Freeze the command names, UUID compatibility constraints, and default timing behavior currently relied on by the mobile app
- Introduce explicit protocol version fields in internal BLE responses and debug logs
- Keep firmware version and BLE protocol version independently trackable

### Phase 2: Transport and GATT Layer Split

- Isolate GAP, advertising, connection lifecycle, MTU, and GATT callback handling into a transport-focused module
- Keep attribute table ownership local to the transport layer
- Expose a narrow interface so upper layers no longer depend on raw ESP BLE event details

### Phase 3: Command Codec Extraction

- Move payload parsing and validation into a dedicated codec/parser module
- Normalize command decoding for pan, tilt, stop, center, and duration-bound movement requests
- Return structured command objects and error codes instead of mixing parse logic with side effects

### Phase 4: Motion Execution Adapter

- Route validated commands through a motion executor that is solely responsible for invoking `hal_servo`
- Centralize default duration handling, safety clamping, and command rejection rules
- Prepare the executor to integrate behavior-state coordination so BLE commands do not fight other runtime behaviors

### Phase 5: Status and Notification Channel

- Add a consistent state-reporting path for connection status, last command result, and motion completion feedback
- Standardize notification payloads so the mobile side can consume acknowledgements and errors predictably
- Leave advanced telemetry optional behind capability flags

### Phase 6: Provisioning and Capability Expansion

- Re-introduce Wi-Fi provisioning on top of the modularized BLE stack instead of mixing it into the motion-control core
- Add capability discovery so clients can identify supported features by firmware/protocol version
- Gate new characteristics behind compatibility checks to preserve the current app flow

### Phase 7: Test and Release Hardening

- Add host-side protocol parsing tests for valid and invalid command payloads
- Add integration validation for advertise-connect-write-disconnect cycles
- Run on-device regression for servo safety, coexistence with Wi-Fi, and app compatibility before promoting from `dev` to release

## Deliverables Expected From The Refactor

- Smaller BLE source files with clear module boundaries
- A documented protocol contract for app and firmware teams
- Safer motion command handling and clearer failure reasons
- A stable path to add provisioning and status features without reopening the current monolithic BLE service

## Immediate Next Steps After This Commit

- Split `ble_service.c` into transport, codec, and executor modules without changing the external command behavior
- Add a dedicated BLE protocol version constant and include it in logs/diagnostics
- Define the first parser test matrix from the current command set

## Validation Target For `v0.0.97dev`

- Firmware version updated to `0.0.97dev`
- Refactor scope and sequence documented in-repo
- Branch prepared for follow-up BLE modularization commits
