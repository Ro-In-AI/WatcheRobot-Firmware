#!/usr/bin/env python3
"""
png2rgba.py — Convert PNG animation frames to RGB565 binary format

Used for pre-decoding animation frames before uploading to device SPIFFS.
Pre-decoded frames eliminate runtime PNG decoding overhead, enabling 30fps playback.

Usage:
    python tools/png2rgba.py --input firmware/s3/spiffs/anim/speaking/ --output out/speaking/
    python tools/png2rgba.py --input firmware/s3/spiffs/anim/ --output out/ --all

Output: *.rgb565 files (raw binary, 412x412x2 bytes per frame)
"""

import argparse
import struct
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("ERROR: Pillow not installed. Run: pip install Pillow")
    sys.exit(1)

TARGET_WIDTH  = 412
TARGET_HEIGHT = 412


def png_to_rgb565(png_path: Path, output_path: Path) -> int:
    """Convert a single PNG to RGB565 binary. Returns file size in bytes."""
    with Image.open(png_path) as img:
        if img.size != (TARGET_WIDTH, TARGET_HEIGHT):
            img = img.resize((TARGET_WIDTH, TARGET_HEIGHT), Image.LANCZOS)

        img = img.convert("RGBA")
        pixels = list(img.getdata())

    output_path.parent.mkdir(parents=True, exist_ok=True)

    with open(output_path, "wb") as f:
        for r, g, b, _a in pixels:
            # RGB565: 5 bits R, 6 bits G, 5 bits B (big-endian)
            rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            f.write(struct.pack(">H", rgb565))

    size = output_path.stat().st_size
    print(f"  {png_path.name} → {output_path.name} ({size // 1024} KB)")
    return size


def convert_directory(input_dir: Path, output_dir: Path) -> None:
    png_files = sorted(input_dir.glob("*.png"))
    if not png_files:
        print(f"  No PNG files found in {input_dir}")
        return

    total = 0
    for png_file in png_files:
        out_file = output_dir / png_file.with_suffix(".rgb565").name
        total += png_to_rgb565(png_file, out_file)

    print(f"  Total: {len(png_files)} frames, {total // 1024} KB")


def main():
    parser = argparse.ArgumentParser(description="Convert PNG frames to RGB565 binary")
    parser.add_argument("--input",  "-i", required=True, help="Input PNG file or directory")
    parser.add_argument("--output", "-o", required=True, help="Output directory")
    parser.add_argument("--all",    "-a", action="store_true",
                        help="Process all subdirectories (one per animation type)")
    args = parser.parse_args()

    input_path  = Path(args.input)
    output_path = Path(args.output)

    if args.all:
        # Process each subdirectory as an animation type
        for subdir in sorted(input_path.iterdir()):
            if subdir.is_dir():
                print(f"\n[{subdir.name}]")
                convert_directory(subdir, output_path / subdir.name)
    elif input_path.is_dir():
        print(f"\n[{input_path.name}]")
        convert_directory(input_path, output_path)
    elif input_path.is_file():
        out_file = output_path / input_path.with_suffix(".rgb565").name
        png_to_rgb565(input_path, out_file)
    else:
        print(f"ERROR: Input path not found: {input_path}")
        sys.exit(1)

    print("\nDone.")


if __name__ == "__main__":
    main()
