#!/usr/bin/env python3
"""
Generate anim_manifest.bin and first-frame raw565 previews from PNG assets.

Requires Pillow:
    python -m pip install Pillow
"""

from __future__ import annotations

import argparse
import shutil
import struct
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError as exc:  # pragma: no cover - runtime dependency check
    raise SystemExit("Pillow is required. Install it with: python -m pip install Pillow") from exc


ANIM_TYPES = [
    "boot",
    "happy",
    "error",
    "speaking",
    "listening",
    "processing",
    "standby",
    "thinking",
    "custom1",
    "custom2",
    "custom3",
]

MAX_FRAMES = 24
PATH_LEN = 96
NAME_LEN = 24
MANIFEST_MAGIC = 0x4D494E41
MANIFEST_VERSION = 1
IMPORT_MAPPINGS = (
    ("watcher-boot", "boot"),
    ("watcher-error", "error"),
    ("watcher-happy", "happy"),
    ("watcher-listening", "listening"),
    ("watcher-processing", "processing"),
    ("watcher-processing2", "custom3"),
    ("watcher-speaking", "speaking"),
    ("watcher-standby", "standby"),
    ("watcher-thinking", "thinking"),
)


def encode_c_string(value: str, size: int) -> bytes:
    data = value.encode("utf-8")
    if len(data) >= size:
        raise ValueError(f"Value too long for fixed field ({size} bytes): {value}")
    return data + b"\0" * (size - len(data))


def rgba_to_rgb565(image: Image.Image) -> bytes:
    rgba = image.convert("RGBA")
    payload = bytearray()
    for r, g, b, a in rgba.getdata():
        if a != 255:
            r = (r * a) // 255
            g = (g * a) // 255
            b = (b * a) // 255
        value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        payload += struct.pack("<H", value)
    return bytes(payload)


def discover_frames(spiffs_dir: Path, prefix: str) -> list[Path]:
    matches = []
    for path in spiffs_dir.glob(f"{prefix}*.png"):
        stem = path.stem
        suffix = stem[len(prefix) :]
        try:
            index = int(suffix) if suffix else 0
        except ValueError:
            continue
        matches.append((index, path))
    matches.sort(key=lambda item: item[0])
    return [path for _, path in matches[:MAX_FRAMES]]


def import_frames(import_dir: Path, spiffs_dir: Path) -> None:
    if not import_dir.is_dir():
        raise SystemExit(f"Import directory does not exist: {import_dir}")

    for _, target_prefix in IMPORT_MAPPINGS:
        for existing in spiffs_dir.glob(f"{target_prefix}*.png"):
            existing.unlink()

    for source_name, target_prefix in IMPORT_MAPPINGS:
        source_dir = import_dir / source_name
        if not source_dir.is_dir():
            raise SystemExit(f"Animation source directory does not exist: {source_dir}")

        frames = sorted(source_dir.glob("*.png"))
        if not frames:
            raise SystemExit(f"No PNG frames found in animation source: {source_dir}")

        for index, frame in enumerate(frames, start=1):
            shutil.copy2(frame, spiffs_dir / f"{target_prefix}{index}.png")

        print(f"Imported {len(frames)} frames: {source_name} -> {target_prefix}")


def build_manifest(spiffs_dir: Path, output_dir: Path, default_fps: int) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    entries = []

    for type_id, name in enumerate(ANIM_TYPES):
        frames = discover_frames(spiffs_dir, name)
        if not frames:
            continue

        with Image.open(frames[0]) as first_img:
            width, height = first_img.size
            raw565 = rgba_to_rgb565(first_img)

        raw_name = f"{name}_first.raw565"
        raw_path = output_dir / raw_name
        raw_path.write_bytes(raw565)

        frame_paths = []
        for frame in frames:
            frame_paths.append(frame.relative_to(spiffs_dir).as_posix())
        while len(frame_paths) < MAX_FRAMES:
            frame_paths.append("")

        entry = struct.pack(
            f"<HHHHHB3x{NAME_LEN}s{PATH_LEN}s",
            type_id,
            width,
            height,
            default_fps,
            len(frames),
            1,
            encode_c_string(name, NAME_LEN),
            encode_c_string(f"anim/{raw_name}", PATH_LEN),
        )
        entry += b"".join(encode_c_string(path, PATH_LEN) for path in frame_paths)
        entries.append(entry)

    manifest_path = output_dir / "anim_manifest.bin"
    manifest_path.write_bytes(
        struct.pack("<IHH", MANIFEST_MAGIC, MANIFEST_VERSION, len(entries)) + b"".join(entries)
    )

    print(f"Wrote {manifest_path}")
    print(f"Generated {len(entries)} manifest entries in {output_dir}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spiffs-dir", default="spiffs", help="Path to the SPIFFS asset directory")
    parser.add_argument("--output-dir", default="spiffs/anim", help="Where generated assets should be written")
    parser.add_argument("--import-dir", help="Directory containing watcher0327png-style animation folders")
    parser.add_argument("--fps", type=int, default=10, help="Default FPS stored in manifest entries")
    args = parser.parse_args()

    spiffs_dir = Path(args.spiffs_dir).resolve()
    output_dir = Path(args.output_dir).resolve()
    if not spiffs_dir.is_dir():
        raise SystemExit(f"SPIFFS directory does not exist: {spiffs_dir}")

    if args.import_dir:
        import_frames(Path(args.import_dir).resolve(), spiffs_dir)

    build_manifest(spiffs_dir, output_dir, args.fps)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
