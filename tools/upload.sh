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

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SKETCH_DIR="$(cd "$(dirname "$0")/../Arduino/solar-logger" && pwd)"
HANDSHAKE_FILE="$REPO_ROOT/.upload-in-progress"


# Always remove the coordination file, including on upload failure, Ctrl-C,
# or termination. Leaving it behind would make the logger stay disconnected.
cleanup_handshake() {
  if [[ -e "$HANDSHAKE_FILE" ]]; then
    if rm -f "$HANDSHAKE_FILE"; then
      echo "[SERIAL] Upload handshake removed."
    else
      echo "[SERIAL] ERROR: Could not remove $HANDSHAKE_FILE" >&2
      return 1
    fi
  fi
}

trap cleanup_handshake EXIT INT TERM


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
# COMPILE
# ---------------------------------------------------------------------------
#
# Compile before requesting serial access. Compilation does not need USB
# Serial, so the logger can continue recording until the finished binary is
# ready to upload.

echo "[BUILD] Compiling firmware..."

arduino-cli compile \
  --fqbn "$FQBN" \
  "$SKETCH_DIR"

echo
echo "[BUILD] COMPILE SUCCESSFUL"
echo


# ---------------------------------------------------------------------------
# REQUEST EXCLUSIVE SERIAL ACCESS
# ---------------------------------------------------------------------------

echo "[SERIAL] Requesting exclusive serial access from logger..."
printf '%s\n' "REQUESTED" > "$HANDSHAKE_FILE"
echo "[SERIAL] Handshake created: $HANDSHAKE_FILE"
echo "[SERIAL] Waiting for logger acknowledgement..."

handshake_state=""
released="false"

for ((attempt = 1; attempt <= 50; attempt++)); do
  if [[ -f "$HANDSHAKE_FILE" ]]; then
    IFS= read -r handshake_state < "$HANDSHAKE_FILE" || handshake_state=""

    if [[ "$handshake_state" == "RELEASED" ]]; then
      released="true"
      break
    fi
  fi

  sleep 0.1
done

if [[ "$released" != "true" ]]; then
  echo "[SERIAL] ERROR: Logger did not acknowledge RELEASED within 5 seconds." >&2
  echo "[SERIAL] ERROR: Refusing to start esptool." >&2
  exit 1
fi

echo "[SERIAL] Logger acknowledged RELEASED: ALL OK"


# ---------------------------------------------------------------------------
# FIND THE ESP32 AFTER RELEASE
# ---------------------------------------------------------------------------

# The board can receive a different usbmodem number after reset, so discover
# the current port only after Python has released the serial device.
shopt -s nullglob
USB_MODEM_PORTS=(/dev/cu.usbmodem*)

if (( ${#USB_MODEM_PORTS[@]} == 0 )); then
  echo "[SERIAL] ERROR: No /dev/cu.usbmodem* port found after release." >&2
  exit 1
fi

if (( ${#USB_MODEM_PORTS[@]} > 1 )); then
  echo "[SERIAL] WARNING: Multiple USB modem ports found; using ${USB_MODEM_PORTS[0]}."
fi

PORT="${USB_MODEM_PORTS[0]}"

echo "[SERIAL] Upload port: $PORT"
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