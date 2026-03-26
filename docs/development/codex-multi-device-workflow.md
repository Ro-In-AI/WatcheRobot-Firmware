# Codex Multi-Device Workflow

Use Codex as the control plane for embedded development. Share logical device aliases with the team and keep the local serial port mapping on each machine.

---

## Core Rules

- One feature per git worktree
- One writing agent per worktree
- One logical device alias per active lane
- Never share the default `build/` directory across concurrent feature work
- Never rely on automatic COM selection when multiple devices are attached
- Keep raw `COM` values in `.codex/local/device-map.toml` only

---

## Device Map

Copy the tracked template:

```powershell
Copy-Item .\.codex\device-map.example.toml .\.codex\local\device-map.toml
```

Then edit the local file per machine:

```toml
[devices.s3-a]
firmware = "s3"
port = "COM31"
```

The alias `s3-a` stays stable across the team. Only the local `port` changes.

---

## Preferred Lane Command

Use the repo-level lane runner so Codex and humans both operate on the same fields:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\run-lane.ps1 `
  -Firmware s3 `
  -Feature voice-v015 `
  -DeviceAlias s3-a
```

What it does:

- Resolves `s3-a` from `.codex/local/device-map.toml`
- Chooses a dedicated build directory such as `firmware/s3/build-s3-a`
- Records local lane metadata under `.codex/local/lanes/`
- Starts the existing firmware-specific `flash-monitor.ps1` with an explicit `-Port`
- By default captures a bounded monitor session and returns automatically instead of leaving `idf.py monitor` open forever
- Stores serial output under `.codex/local/logs/`

Optional flags:

```powershell
.\tools\run-lane.cmd -Firmware s3 -Feature camera-smoke -DeviceAlias s3-b -DryRun
.\tools\run-lane.cmd -Firmware s3 -Feature ws-fix -DeviceAlias s3-c -NoBuild
.\tools\run-lane.cmd -Firmware s3 -Feature ws-fix -DeviceAlias s3-c -MonitorSeconds 10 -MonitorMaxLines 120
.\tools\run-lane.cmd -Firmware s3 -Feature ws-fix -DeviceAlias s3-c -NoMonitor
```

---

## Shared Lane Table

Keep the shared lane table free of raw COM ports. A lane should contain:

- `operator`
- `firmware`
- `feature`
- `worktree`
- `branch`
- `device_alias`
- `build_dir`
- `test_profile`
- `status`

See `.codex/lanes.example.yaml` for a tracked example.

---

## S3 Flashing Notes

`firmware/s3/tools/flash-monitor.ps1` still supports explicit ports:

```powershell
powershell -ExecutionPolicy Bypass -File .\firmware\s3\tools\flash-monitor.ps1 -Port COM31 -BuildPath .\firmware\s3\build-s3-a
```

It also supports alias resolution directly:

```powershell
powershell -ExecutionPolicy Bypass -File .\firmware\s3\tools\flash-monitor.ps1 -DeviceAlias s3-a -BuildPath .\firmware\s3\build-s3-a
```

If multiple serial ports are visible and no port or alias is provided, the script now fails fast instead of silently picking one. Use `-AutoSelectHighestPort` only as a temporary single-user escape hatch.

---

## Camera / Discovery Test Limitation

`firmware/s3/tools/ws_camera_gateway_test.py` is still a single-device harness. Run camera and discovery gateway tests serially, not in parallel across three devices on the same LAN.
