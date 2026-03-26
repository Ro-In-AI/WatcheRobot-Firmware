# Codex Workspace Conventions

This directory holds tracked templates for Codex-based firmware workflows.

## Local-Only Files

Create your machine-specific files under `.codex/local/`.

- `.codex/local/device-map.toml`
  Maps logical device aliases like `s3-a` to the local serial port on this machine.
- `.codex/local/lanes/`
  Runtime lane metadata emitted by `tools/run-lane.ps1`.
- `.codex/local/logs/`
  Optional transcripts and session logs emitted by `tools/run-lane.ps1`.

Everything under `.codex/local/` is ignored by git.

## Tracked Templates

- `device-map.example.toml`
  Copy this to `.codex/local/device-map.toml` and fill in your own ports.
- `lanes.example.yaml`
  Shared lane table example. Keep only device aliases here, never raw COM ports.
