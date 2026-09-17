#!/usr/bin/env bash

# Watch the XIAO's USB port appear and disappear.
#
# The board exposes USB only during the rendezvous window that follows each
# autonomous timer wake, so this is how you see the autonomous cycle from the
# host without opening the port or disturbing the board. It only stats device
# files; nothing here connects, claims a session, or sends a command.
#
# Default output is one line per CHANGE, with how long the previous state
# lasted. That duration is the measurement worth having: it is the rendezvous
# window when the port is present, and the sleep interval when it is absent.
#
#   16:51:45  GONE    /dev/cu.usbmodem1101      after 47.0s
#   16:51:49  PRESENT /dev/cu.usbmodem1101
#   16:51:59  GONE    /dev/cu.usbmodem1101      after 10.0s
#
# Deliberately bash rather than zsh: an unmatched glob is an error in zsh, so
# the obvious `ls /dev/cu.usbmodem* || echo NONE` prints a shell error on top of
# its own fallback on every tick. In bash the glob passes through, ls fails
# quietly, and the fallback works.
#
# Usage:
#   tools/watch-port.sh              one line per change (default)
#   tools/watch-port.sh --all        one line per poll, like a raw poll loop
#   tools/watch-port.sh --interval 0.5
#
# Control-C to stop.

set -u

POLL_SECONDS="0.2"
MODE="changes"
PORT_GLOB="/dev/cu.usbmodem*"

while (( $# > 0 )); do
  case "$1" in
    --all)
      MODE="all"
      shift
      ;;
    --interval)
      if (( $# < 2 )); then
        echo "[ERROR] --interval needs a value." >&2
        exit 1
      fi
      POLL_SECONDS="$2"
      shift 2
      ;;
    -h|--help)
      sed -n '3,30p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "[ERROR] Unknown option: $1" >&2
      echo "[ERROR] Usage: tools/watch-port.sh [--all] [--interval SECONDS]" >&2
      exit 1
      ;;
  esac
done


on_interrupt() {
  echo
  echo "[WATCH] Stopped."
  exit 130
}

trap on_interrupt INT


echo "[WATCH] Polling $PORT_GLOB every ${POLL_SECONDS}s. Control-C to stop."

if [[ "$MODE" == "changes" ]]; then
  echo "[WATCH] Printing state changes only, with how long the previous state lasted."
fi

echo


last_state=""
last_port=""
state_started=$SECONDS

while true; do
  # In bash an unmatched glob stays literal, so ls simply fails and the
  # substitution is empty. No shell error, unlike zsh.
  port="$(ls -d /dev/cu.usbmodem* 2>/dev/null | head -n 1)"

  if [[ -n "$port" ]]; then
    state="PRESENT"
    last_port="$port"
  else
    state="GONE"
  fi

  stamp="$(date '+%H:%M:%S')"

  if [[ "$MODE" == "all" ]]; then
    echo "$stamp  ${port:-NO USB PORT}"

  elif [[ "$state" != "$last_state" ]]; then
    if [[ -z "$last_state" ]]; then
      # First observation. There is no previous state to time.
      printf '%s  %-7s %s\n' "$stamp" "$state" "${port:-(none yet)}"
    else
      held=$(( SECONDS - state_started ))
      printf '%s  %-7s %-24s after %ds\n' \
        "$stamp" "$state" "${port:-$last_port}" "$held"
    fi

    last_state="$state"
    state_started=$SECONDS
  fi

  sleep "$POLL_SECONDS"
done
