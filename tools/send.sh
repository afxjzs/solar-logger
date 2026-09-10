#!/usr/bin/env bash

# Send one newline-terminated firmware command without opening a second Serial
# owner when the Python logger is already running.
set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
COMMAND_FILE="$REPO_ROOT/.serial-command"
UPLOAD_HANDSHAKE_FILE="$REPO_ROOT/.upload-in-progress"
BAUD_RATE=115200

if (( $# == 0 )); then
  echo "[ERROR] Usage: tools/send.sh COMMAND [ARGUMENT ...]" >&2
  exit 1
fi

COMMAND="$*"

if [[ -e "$UPLOAD_HANDSHAKE_FILE" ]]; then
  echo "[ERROR] Firmware upload is in progress. Command not sent." >&2
  exit 1
fi

echo "[COMMAND] BMW Solar Logger command sender"
echo "[COMMAND] Command: $COMMAND"

LOGGER_RUNNING="false"
if pgrep -f '(^|[[:space:]/])app/solar_logger\.py([[:space:]]|$)' >/dev/null; then
  LOGGER_RUNNING="true"
fi

if [[ "$LOGGER_RUNNING" == "true" ]]; then
  echo "[SERIAL] Logger detected. Handing command to logger."

  # noclobber makes creation fail instead of overwriting another pending
  # command. The logger is the only process that owns Serial in this path.
  set -C
  if ! printf '%s\n' "$COMMAND" > "$COMMAND_FILE"; then
    set +C
    echo "[COMMAND] ERROR: A command is already pending." >&2
    exit 1
  fi
  set +C

  echo "[COMMAND] Queued for logger: ALL OK"
  exit 0
fi

echo "[SERIAL] Logger not running. Using direct serial access."

shopt -s nullglob
USB_MODEM_PORTS=(/dev/cu.usbmodem*)

if (( ${#USB_MODEM_PORTS[@]} == 0 )); then
  echo "[SERIAL] ERROR: No /dev/cu.usbmodem* port found." >&2
  exit 1
fi

if (( ${#USB_MODEM_PORTS[@]} > 1 )); then
  echo "[SERIAL] WARNING: Multiple USB modem ports found; using ${USB_MODEM_PORTS[0]}."
fi

PORT="${USB_MODEM_PORTS[0]}"

echo "[SERIAL] Port: $PORT"

# macOS stty configures the discovered device; the redirection opens it only
# for this one command and closes it immediately afterward.
if ! stty -f "$PORT" "$BAUD_RATE" cs8 -cstopb -parenb; then
  echo "[SERIAL] ERROR: Could not configure $PORT at $BAUD_RATE baud." >&2
  exit 1
fi

if ! printf '%s\n' "$COMMAND" > "$PORT"; then
  echo "[COMMAND] ERROR: Could not send command to $PORT." >&2
  exit 1
fi

echo "[COMMAND] Sent: $COMMAND"
echo "[COMMAND] ALL OK"
