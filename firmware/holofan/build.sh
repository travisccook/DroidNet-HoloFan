#!/usr/bin/env bash
# Build the HoloFan firmware, then check that the image fits an OTA slot and
# that the shared mesh core makes no libatomic calls (the ESP32-C2 has no
# atomic instructions).
#   ./build.sh            build into build/
# Needs ESP-IDF at commit 11edebdbbe (release/v5.4, the ESP-IDF build the stock
# fan firmware used; README "Building"). IDF_PATH defaults to ~/esp/esp-idf.
# IDF_TOOLS_PATH is passed on only if you set it (ESP-IDF's default is ~/.espressif).
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
export IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
if [ -n "${IDF_TOOLS_PATH:-}" ]; then export IDF_TOOLS_PATH; fi
WANT_IDF=11edebdbbe
GOT_IDF=$(git -C "$IDF_PATH" rev-parse HEAD 2>/dev/null || echo unknown)
case "$GOT_IDF" in
  "$WANT_IDF"*) ;;
  *) echo "WARNING: ESP-IDF at $IDF_PATH is at ${GOT_IDF:0:10}, not $WANT_IDF (the tested build)" ;;
esac
# shellcheck disable=SC1091
source "$IDF_PATH/export.sh" >/dev/null 2>&1 || { echo "export.sh failed (check IDF_PATH / IDF_TOOLS_PATH)"; exit 2; }
cd "$HERE"
[ -f sdkconfig ] || idf.py set-target esp32c2 >/dev/null
idf.py build

BIN=build/holofan.bin
SIZE=$(wc -c <"$BIN" | tr -d ' ')
SLOT_MAX=$((0xF0000))            # partitions.csv: two OTA slots in 2 MB after 0x10000
echo "image: $BIN  $SIZE bytes (OTA slot budget $SLOT_MAX)"
[ "$SIZE" -le "$SLOT_MAX" ] || { echo "FAIL: image too large for an OTA slot"; exit 3; }

ATOMIC=$(riscv32-esp-elf-nm -u build/esp-idf/wcbcore/libwcbcore.a | grep -c '__atomic_' || true)
echo "libatomic calls in wcbcore: $ATOMIC"
[ "$ATOMIC" -eq 0 ] || { riscv32-esp-elf-nm -u build/esp-idf/wcbcore/libwcbcore.a | grep '__atomic_'; echo "FAIL: libatomic calls"; exit 4; }
echo "BUILD OK"
