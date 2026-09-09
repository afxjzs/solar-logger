#!/usr/bin/env bash

# Stop immediately if any command fails.
#
# Without this, a shell script normally keeps executing after many kinds
# of errors. For firmware deployment that is exactly what we do not want.
set -e


# ---------------------------------------------------------------------------
# BMW SOLAR LOGGER - COMPILE + UPLOAD
# ---------------------------------------------------------------------------

FQBN="esp32:esp32:XIAO_ESP32C3"

SKETCH_DIR="$(cd "$(dirname "$0")/../Arduino/solar-logger" && pwd)"


echo
echo "============================================================"
echo "BMW SOLAR LOGGER - BUILD + UPLOAD"
echo "============================================================"
echo

echo "[BUILD] Sketch:"
echo "        $SKETCH_DIR"
echo

echo "[BUILD] Target:"
echo "        $FQBN"
echo


# ---------------------------------------------------------------------------
# FIND THE ESP32
# ---------------------------------------------------------------------------
#
# For the moment we will use the known USB port.
#
# Soon we will replace this with automatic board discovery so we do not
# depend on the Mac always assigning usbmodem1101.

PORT="/dev/cu.usbmodem1101"

echo "[SERIAL] Upload port:"
echo "         $PORT"
echo


# ---------------------------------------------------------------------------
# COMPILE
# ---------------------------------------------------------------------------

echo "[BUILD] Compiling firmware..."

arduino-cli compile \
  --fqbn "$FQBN" \
  "$SKETCH_DIR"

echo
echo "[BUILD] COMPILE SUCCESSFUL"
echo


# ---------------------------------------------------------------------------
# UPLOAD
# ---------------------------------------------------------------------------

echo "[UPLOAD] Flashing XIAO ESP32-C3..."

arduino-cli upload \
  --fqbn "$FQBN" \
  --port "$PORT" \
  "$SKETCH_DIR"

echo
echo "============================================================"
echo "UPLOAD COMPLETE: ALL OK"
echo "============================================================"
echo