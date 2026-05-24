#!/usr/bin/env bash
# Copy esp32/src/* to a connected ESP32 via mpremote.
#
# Prerequisite: pip install mpremote
#
# Usage:
#   esp32/scripts/install.sh /dev/ttyUSB0    # Linux
#   esp32/scripts/install.sh COM7            # Windows (run under git-bash)

set -euo pipefail

PORT="${1:-}"
if [[ -z "$PORT" ]]; then
    echo "Usage: $0 <port>"
    echo "  Linux:   $0 /dev/ttyUSB0"
    echo "  macOS:   $0 /dev/tty.SLAB_USBtoUART"
    echo "  Windows: $0 COM7"
    exit 1
fi

# Resolve to this script's repo root regardless of cwd
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/../src"

if [[ ! -d "$SRC_DIR" ]]; then
    echo "Error: $SRC_DIR not found" >&2
    exit 1
fi

if ! command -v mpremote >/dev/null; then
    echo "Error: mpremote not installed. Run: pip install mpremote" >&2
    exit 1
fi

echo "==> Copying esp32/src/* to $PORT..."
# Copy directory contents (the trailing '.' matters; copies *files inside* src)
mpremote connect "$PORT" cp -r "$SRC_DIR/." :
echo
echo "==> Verifying file listing on device:"
mpremote connect "$PORT" ls
echo
echo "Install complete. Soft-reboot the board to run main.py:"
echo "  mpremote connect $PORT reset"
