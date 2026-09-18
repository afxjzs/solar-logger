#!/usr/bin/env bash

# Send one firmware command and print what the firmware says back.
#
# THIS IS A THIN WRAPPER. The protocol lives in the shared host module:
#
#   app/device_session.py   port discovery, rendezvous waiting, acknowledgement
#   app/device_tool.py      the command-line front end this calls
#
# app/solar_logger.py and tools/upload.sh speak the same protocol through the
# same module, so there is one implementation of it rather than three.
#
#
# WHAT IS PRESERVED FROM THE PURE-SHELL VERSION
#
#   - Writing bytes is NOT acknowledgement. Success requires the firmware's
#     machine CMD_RESULT,<command>,OK; human diagnostics are not protocol
#     state. A RESULT whose CMD_ACK was lost still counts, and the missing ACK
#     is printed as a warning rather than passed off as clean (D-045).
#   - The response is printed, ending on a quiet period or a hard cap, with the
#     stop reason always stated and truncation warned about.
#   - LOGGER STORAGE DUMP gets the longer capture window.
#   - When app/solar_logger.py owns the port, the command is queued through
#     .serial-command and no second reader is ever opened.
#   - Refuses to send while .upload-in-progress exists.
#
#
#
# WAITING IS THE DEFAULT
#
#   The board's NORMAL state is autonomous sleep, so /dev/cu.usbmodem* being
#   absent is ordinary rather than exceptional. A plain send therefore waits
#   for the next USB rendezvous:
#
#       tools/send.sh LOGGER STORAGE INFO
#
#           board awake   -> sent now
#           board asleep  -> waits for the next rendezvous, then sends
#
#   --no-wait asks for immediate failure instead, for scripts that need it.
#   --wait still exists and is accepted; it is now the default and says so.
#
#   THE EXCEPTION, and the reason this is not a blanket rule:
#
#       LOGGER SESSION KEEPALIVE
#       LOGGER SESSION RELEASE
#
#   address a session that is ALREADY held. A session lives on one transport,
#   so once the port is gone the session is over - the firmware's 15-second
#   lease expires by itself and the board sleeps again with no help from us.
#   Waiting would deliver a stale RELEASE to a board that has since woken into
#   a NEW rendezvous, possibly one another process just claimed. These two
#   never wait, and an explicit --wait on them is refused rather than ignored.
#
#   LOGGER SESSION HOLD is not in that set: it CREATES a session, so any
#   rendezvous will do and waiting is exactly right.
#
#   The rule lives in app/device_session.py (requires_live_session), so all
#   three tools classify commands identically.
#
# A one-shot send NEVER takes a session of its own. LOGGER SESSION HOLD can be
# sent like any other command, but nothing here renews it, so the firmware's
# 15-second lease expires on its own. That is deliberate: a command typed once
# must not be able to leave the board awake indefinitely.
#
# Examples:
#
#   tools/send.sh LOGGER AUTONOMOUS STATUS
#   tools/send.sh LOGGER STORAGE INFO
#   tools/send.sh LOGGER SESSION HOLD
#   tools/send.sh --no-wait STATUS
#
# Exit status: 0 acknowledged and completed, 1 not, 2 request refused,
# 130 cancelled.

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEVICE_TOOL="$REPO_ROOT/app/device_tool.py"

if [[ ! -f "$DEVICE_TOOL" ]]; then
  echo "[ERROR] Missing $DEVICE_TOOL" >&2
  exit 1
fi

# Prefer the project environment's interpreter so pyserial is present without
# the user having to activate anything. Falls back to python3, which is enough
# on a machine where pyserial is installed globally.
PYTHON="$REPO_ROOT/.venv/bin/python"

if [[ ! -x "$PYTHON" ]]; then
  PYTHON="python3"
fi

if (( $# == 0 )); then
  echo "[ERROR] Usage: tools/send.sh [--wait|--no-wait] COMMAND [ARGUMENT ...]" >&2
  exit 1
fi

# Pass through the options device_tool understands, then the command words.
TOOL_ARGS=()

while (( $# > 0 )); do
  case "$1" in
    --wait|--no-wait)
      TOOL_ARGS+=("$1")
      shift
      ;;
    --timeout|--capture-seconds|--idle-seconds)
      if (( $# < 2 )); then
        echo "[ERROR] $1 needs a value." >&2
        exit 1
      fi
      TOOL_ARGS+=("$1" "$2")
      shift 2
      ;;
    --)
      shift
      break
      ;;
    *)
      break
      ;;
  esac
done

if (( $# == 0 )); then
  echo "[ERROR] No command given." >&2
  exit 1
fi

exec "$PYTHON" "$DEVICE_TOOL" send "${TOOL_ARGS[@]}" -- "$@"
