#!/usr/bin/env bash
# flash.sh — Build and flash WatcheRobot S3 firmware
#
# Usage:
#   ./tools/flash.sh            # build + flash + monitor (auto-detect port)
#   ./tools/flash.sh COM3       # Windows: specify port
#   ./tools/flash.sh /dev/ttyUSB0  # Linux: specify port
#   ./tools/flash.sh --build-only  # build without flashing

set -euo pipefail

FIRMWARE_DIR="$(cd "$(dirname "$0")/../firmware/s3" && pwd)"
PORT="${1:-}"
BUILD_ONLY=false

if [[ "${PORT}" == "--build-only" ]]; then
    BUILD_ONLY=true
    PORT=""
fi

echo "=== WatcheRobot Firmware Build ==="
echo "Firmware dir: ${FIRMWARE_DIR}"

cd "${FIRMWARE_DIR}"

# Build
echo ""
echo "--- Building ---"
idf.py build

if [[ "${BUILD_ONLY}" == "true" ]]; then
    echo "Build complete (--build-only mode, skipping flash)."
    exit 0
fi

# Flash
echo ""
echo "--- Flashing ---"
if [[ -n "${PORT}" ]]; then
    idf.py -p "${PORT}" flash monitor
else
    # Auto-detect port (Linux/macOS)
    if [[ "$(uname)" == "Linux" ]]; then
        DETECTED_PORT=$(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | head -1 || true)
        if [[ -n "${DETECTED_PORT}" ]]; then
            echo "Auto-detected port: ${DETECTED_PORT}"
            idf.py -p "${DETECTED_PORT}" flash monitor
        else
            echo "ERROR: No serial port detected. Specify port as argument."
            echo "Usage: ./tools/flash.sh /dev/ttyUSB0"
            exit 1
        fi
    else
        echo "ERROR: Cannot auto-detect port on this OS. Specify port as argument."
        echo "Usage: ./tools/flash.sh COM3   (Windows)"
        exit 1
    fi
fi
