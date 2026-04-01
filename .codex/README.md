# Codex Workspace Conventions

This directory holds tracked templates for Codex-based firmware workflows.

## Local-Only Files

Create your machine-specific files under a shared local path, then point every worktree at it with `CODEX_DEVICE_MAP_PATH`.

- `$env:USERPROFILE\.codex\local\device-map.toml`
  Maps logical device aliases like `s3-a` to the local serial port on this machine.
- `.codex/local/lanes/`
  Runtime lane metadata emitted by `tools/run-lane.ps1`.
- `.codex/local/logs/`
  Optional transcripts and session logs emitted by `tools/run-lane.ps1`.

The device map should live outside individual worktrees so every Codex checkout sees the same alias-to-port mapping.

## Tracked Templates

- `device-map.example.toml`
  Copy this to `$env:USERPROFILE\.codex\local\device-map.toml` and fill in your own ports.
- `lanes.example.yaml`
  Shared lane table example. Keep only device aliases here, never raw COM ports.
