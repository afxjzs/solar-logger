#!/usr/bin/env bash

# Stop immediately if any command fails.
#
# Without this, a shell script normally keeps executing after many kinds
# of errors. For firmware deployment that is exactly what we do not want.
#
# The upload attempt itself is deliberately exempted, because a failed attempt
# against a sleeping board is an expected state rather than an error. See the
# upload loop below.
set -e


# ---------------------------------------------------------------------------
# BMW SOLAR LOGGER - COMPILE + UPLOAD
# ---------------------------------------------------------------------------
#
# AUTONOMOUS-SLEEP AWARE.
#
# The board can now put itself into deep sleep and expose USB only during the
# rendezvous window that follows each timer wake. So "no /dev/cu.usbmodem*
# exists" is an ordinary state, not a failure.
#
# This script therefore compiles exactly once, then waits for the board to
# appear and uploads the binary it already built. If a rendezvous is missed
# part way through, it waits for the next one and retries THE SAME ARTIFACT.
# It never recompiles on a retry.

FQBN="esp32:esp32:XIAO_ESP32C3"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SKETCH_DIR="$(cd "$(dirname "$0")/../Arduino/solar-logger" && pwd)"
HANDSHAKE_FILE="$REPO_ROOT/.upload-in-progress"

# How often to look for the port while waiting for a rendezvous. Fast enough to
# catch a short window, slow enough not to busy-loop.
PORT_POLL_INTERVAL_SECONDS="${UPLOAD_PORT_POLL_SECONDS:-0.2}"

# Settle time after a failed attempt, so a board that is on its way back to
# sleep is not immediately hammered with another esptool handshake.
RETRY_SETTLE_SECONDS="${UPLOAD_RETRY_SETTLE_SECONDS:-1}"

# 0 means keep waiting until the user cancels. Waiting for a sleeping board can
# legitimately take longer than one autonomous cadence, so there is no short
# default timeout. Control-C is the documented way to stop.
MAX_ATTEMPTS="${UPLOAD_MAX_ATTEMPTS:-0}"

# How often to reprint "still waiting" while polling, in seconds. The poll
# itself stays quiet; only this heartbeat prints.
WAIT_HEARTBEAT_SECONDS="${UPLOAD_WAIT_HEARTBEAT_SECONDS:-15}"

# Pinned build directory. Compiling into it and uploading out of it is what
# makes "retries use the exact artifact from the initial compile" a property of
# the commands rather than an assumption about arduino-cli's build cache.
BUILD_DIR=""


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

  if [[ -n "$BUILD_DIR" && -d "$BUILD_DIR" ]]; then
    rm -rf "$BUILD_DIR"
  fi
}

on_interrupt() {
  echo
  echo "[UPLOAD] Cancelled by user."
  # Exiting here runs the EXIT trap, so the handshake is still released and the
  # logger is free to reclaim Serial.
  exit 130
}

trap cleanup_handshake EXIT
trap on_interrupt INT
trap 'exit 143' TERM


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
# COMPILE - EXACTLY ONCE
# ---------------------------------------------------------------------------
#
# Compile before requesting serial access. Compilation does not need USB
# Serial, so the logger can continue recording until the finished binary is
# ready to upload.
#
# This is the only compile in the script. Nothing below this point invokes the
# compiler again, however many upload attempts it takes.

BUILD_DIR="$(mktemp -d -t solar-upload-build)"

# ---------------------------------------------------------------------------
# SOURCE REVISION INJECTION
# ---------------------------------------------------------------------------
#
# This is the canonical build, so this is where the image is tied back to the
# source that produced it. The firmware prints it at boot, in STATUS, and on
# the VERSION command.
#
# Everything here is best-effort and every outcome is announced. A missing Git
# repository or a detached worktree must not stop an upload, but it must also
# never produce an image that claims a revision it does not have: the firmware
# prints UNKNOWN when nothing is injected.
#
# git rev-parse runs in a subshell that may fail, so the `set -e` at the top of
# this script is deliberately suppressed for these two commands only.

GIT_REVISION=""

if git -C "$REPO_ROOT" rev-parse --git-dir >/dev/null 2>&1; then
  GIT_REVISION="$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || true)"

  if [[ -z "$GIT_REVISION" ]]; then
    echo "[BUILD] WARNING: Git repository found but HEAD could not be read."
    echo "[BUILD] WARNING: The image will report Revision: UNKNOWN."
  else
    # Only the sketch directory matters here. A dirty docs/ tree does not
    # change what is compiled, and marking the image dirty for it would make
    # the flag mean nothing.
    #
    # BOTH checks are needed. `git diff` sees modified tracked files; a brand
    # new, untracked .h in the sketch directory is compiled into the image and
    # is invisible to it.
    SKETCH_MODIFIED="false"

    if ! git -C "$REPO_ROOT" diff --quiet HEAD -- "$SKETCH_DIR" 2>/dev/null; then
      SKETCH_MODIFIED="true"
    fi

    if [[ -n "$(git -C "$REPO_ROOT" ls-files --others --exclude-standard -- "$SKETCH_DIR" 2>/dev/null)" ]]; then
      SKETCH_MODIFIED="true"
    fi

    if [[ "$SKETCH_MODIFIED" == "true" ]]; then
      GIT_REVISION="${GIT_REVISION}-dirty"
    fi
  fi
else
  echo "[BUILD] WARNING: Not a Git repository. No source revision to inject."
  echo "[BUILD] WARNING: The image will report Revision: UNKNOWN."
fi

BUILD_PROPERTIES=()

if [[ -n "$GIT_REVISION" ]]; then
  # compiler.cpp.extra_flags is empty in the ESP32 platform.txt, so setting it
  # replaces nothing. arduino-cli execs the compiler directly, so the embedded
  # quotes survive as part of one argument and the macro expands to a string
  # literal.
  BUILD_PROPERTIES+=(
    --build-property
    "compiler.cpp.extra_flags=-DFIRMWARE_GIT_REV=\"${GIT_REVISION}\""
  )
  echo "[BUILD] Source revision: $GIT_REVISION"
fi

echo "[BUILD] Compiling..."
echo "[BUILD] Build artifacts: $BUILD_DIR"

arduino-cli compile \
  --fqbn "$FQBN" \
  --output-dir "$BUILD_DIR" \
  "${BUILD_PROPERTIES[@]}" \
  "$SKETCH_DIR"

echo
echo "[BUILD] Compile successful: ALL OK"

if [[ -n "$GIT_REVISION" ]]; then
  echo "[BUILD] Image identity: Revision $GIT_REVISION"
else
  echo "[BUILD] Image identity: Revision UNKNOWN - nothing was injected."
fi
echo "[BUILD] This binary will be used for every upload attempt below."
echo


# ---------------------------------------------------------------------------
# CHECK FOR THE LOGGER AND REQUEST EXCLUSIVE SERIAL ACCESS IF NEEDED
# ---------------------------------------------------------------------------
#
# Unchanged. The handshake is created before the wait loop and released by the
# EXIT trap, so the logger stays off the port for the whole wait, not just for
# the moment esptool is running.

echo "[SERIAL] Checking for running BMW Solar Logger..."

logger_running="false"
if pgrep -f '(^|[[:space:]/])app/solar_logger\.py([[:space:]]|$)' >/dev/null; then
  logger_running="true"
fi

if [[ "$logger_running" == "true" ]]; then
  echo "[SERIAL] Logger process detected."
  echo "[SERIAL] Requesting exclusive serial access..."
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
    # A detected logger may still own or reclaim Serial. Never turn a failed
    # acknowledgment into an upload: that would let esptool race the logger.
    echo "[ERROR] Logger is running but did not acknowledge serial release." >&2
    echo "[ERROR] Refusing to start esptool." >&2
    exit 1
  fi

  echo "[SERIAL] Logger acknowledged RELEASED: ALL OK"
else
  # With no logger process, no one can acknowledge REQUESTED and no logger can
  # own Serial. Upload can proceed without creating a needless handshake.
  echo "[SERIAL] Logger process not detected."
  echo "[SERIAL] No logger handshake required."
fi

echo


# ---------------------------------------------------------------------------
# PORT DISCOVERY - SHARED IMPLEMENTATION
# ---------------------------------------------------------------------------
#
# Delegated to app/device_session.py through app/device_tool.py, so this script
# does not carry a second sleep-aware polling implementation. tools/send.sh and
# app/solar_logger.py wait for a rendezvous through exactly the same code.
#
# The port is rediscovered before every attempt rather than remembered: a board
# that wakes from deep sleep can enumerate under a different usbmodem number,
# so a path that was right one rendezvous ago may name nothing at the next.

DEVICE_TOOL="$REPO_ROOT/app/device_tool.py"

if [[ ! -f "$DEVICE_TOOL" ]]; then
  echo "[ERROR] Missing $DEVICE_TOOL" >&2
  exit 1
fi

PYTHON="$REPO_ROOT/.venv/bin/python"

if [[ ! -x "$PYTHON" ]]; then
  PYTHON="python3"
fi


# Block until a port exists. The shared tool prints its own waiting and
# heartbeat messages to stderr, so they reach the terminal while the port name
# itself comes back on stdout.
#
# The two tunables are passed through explicitly. They used to be read into
# shell variables that nothing used, because waiting moved into device_tool.py
# and nobody noticed the values stopped arriving - so setting either did
# nothing and said nothing.
wait_for_port() {
  "$PYTHON" "$DEVICE_TOOL" wait-port \
    --poll-seconds "$PORT_POLL_INTERVAL_SECONDS" \
    --heartbeat-seconds "$WAIT_HEARTBEAT_SECONDS"
}


# ---------------------------------------------------------------------------
# FAILURE CLASSIFICATION
# ---------------------------------------------------------------------------
#
# Two very different things can make an upload attempt fail, and treating them
# alike would be a bug in either direction: retrying a bad FQBN forever, or
# aborting because a rendezvous closed half a second early.
#
# A. The board was not reachable - it slept, the port vanished, esptool never
#    got a response. Expected during autonomous operation. Wait and retry.
#
# B. A real build, configuration, or tooling error. Print it and stop.
#
# The full upload output is printed on EVERY failure before this runs, so no
# classification decision is made behind the user's back.
is_transient_failure() {
  local output="$1"
  local port="$2"

  # The strongest signal available: whatever the message says, if the port we
  # were just using is gone, the board left. A rendezvous closed under us.
  if [[ ! -e "$port" ]]; then
    echo "[UPLOAD] Reason: the port $port no longer exists."
    return 0
  fi

  # esptool and the serial layer phrase the same underlying condition several
  # ways depending on exactly when the board disappeared.
  local transient_patterns=(
    "No serial data received"
    "Failed to connect"
    "could not open port"
    "Could not open"
    "Failed to open"
    "No such file or directory"
    "Device or resource busy"
    "Serial port .* not found"
    "Timed out waiting for packet"
    "The chip stopped responding"
    "Write timeout"
    "Resource temporarily unavailable"
    "Input/output error"
    "device reports readiness to read but returned no data"
    "Error during Upload: .*ort"
  )

  local pattern
  for pattern in "${transient_patterns[@]}"; do
    if grep -qiE "$pattern" <<< "$output"; then
      echo "[UPLOAD] Reason: matched device-presence pattern \"$pattern\"."
      return 0
    fi
  done

  return 1
}


# ---------------------------------------------------------------------------
# UPLOAD, RETRYING ACROSS RENDEZVOUS WINDOWS
# ---------------------------------------------------------------------------

attempt=0

while true; do
  attempt=$(( attempt + 1 ))

  if (( MAX_ATTEMPTS > 0 && attempt > MAX_ATTEMPTS )); then
    echo "[UPLOAD] NOT ALL OK - gave up after $MAX_ATTEMPTS attempts." >&2
    echo "[UPLOAD] Raise UPLOAD_MAX_ATTEMPTS or use 0 for unlimited." >&2
    exit 1
  fi

  PORT="$(wait_for_port)"

  echo "[UPLOAD] XIAO appeared: $PORT"

  # ------------------------------------------------------------------------
  # Opportunistic session claim, then the handover to esptool.
  # ------------------------------------------------------------------------
  #
  # A rendezvous window is short. Claiming the board buys up to one firmware
  # lease of guaranteed awake time, which makes losing the race between port
  # discovery and esptool's reset much less likely.
  #
  # It is deliberately BEST EFFORT. The firmware refuses a HOLD when autonomous
  # mode is not armed, which is the normal case for an awake board, and that
  # refusal must not fail an upload that was going to work anyway.
  #
  # THE HANDOVER: once esptool runs it asserts DTR/RTS and resets the board
  # into its ROM bootloader. That reset ends the sketch, so the host session and
  # its lease cease to exist at that instant. Nothing renews the lease during an
  # upload and nothing should: there is no sketch left to renew it with. The
  # firmware lease simply becomes irrelevant rather than expiring. After the
  # upload the board boots the new sketch fresh, and if autonomous mode is still
  # armed in NVS it resumes from its cold-boot maintenance window.
  #
  # --no-wait is required, not decorative. device_tool waits for the next USB
  # rendezvous by default, which is right for a person at a terminal and wrong
  # here: this runs inside a retry loop that has already found a port and owns
  # the decision about what to do when the board disappears. Without it, a port
  # that vanished between discovery and this call would block the upload loop
  # indefinitely inside a step documented as best effort.
  if "$PYTHON" "$DEVICE_TOOL" session hold --no-wait --capture-seconds 1 > /dev/null 2>&1; then
    echo "[UPLOAD] Board claimed with a host session; it will stay awake."
  else
    echo "[UPLOAD] No session claimed (board may not be in autonomous mode)."
    echo "[UPLOAD] Proceeding anyway; this is not an error."
  fi

  echo "[UPLOAD] Starting upload attempt $attempt using existing build..."
  echo "[UPLOAD] Artifact: $BUILD_DIR"
  echo "[UPLOAD] esptool now resets the board; the host session ends here."
  echo

  # set -e must not abort the script here: a failed attempt against a sleeping
  # board is an expected state that this loop exists to handle.
  set +e
  upload_output="$(
    arduino-cli upload \
      --fqbn "$FQBN" \
      --port "$PORT" \
      --input-dir "$BUILD_DIR" \
      "$SKETCH_DIR" 2>&1
  )"
  upload_status=$?
  set -e

  # Printed whether the attempt succeeded or failed, so the normal case looks
  # exactly like a plain upload and the failure case hides nothing.
  printf '%s\n' "$upload_output"

  if (( upload_status == 0 )); then
    echo
    echo "============================================================"
    echo "[UPLOAD] Upload successful on attempt $attempt: ALL OK"
    echo "============================================================"
    echo
    exit 0
  fi

  echo
  echo "[UPLOAD] WARNING: Upload attempt $attempt did not complete (exit $upload_status)." >&2

  classification=""
  if classification="$(is_transient_failure "$upload_output" "$PORT")"; then
    echo "$classification" >&2
    echo "[UPLOAD] The board may have returned to deep sleep." >&2
    echo "[UPLOAD] Build is still valid; NOT recompiling." >&2
    echo "[UPLOAD] Waiting for the next USB rendezvous..." >&2
    echo >&2

    # Let a board that is on its way back to sleep finish leaving, instead of
    # catching the tail of the same rendezvous and failing again immediately.
    sleep "$RETRY_SETTLE_SECONDS"
    continue
  fi

  # Not recognizable as a device-presence problem. The full output is already
  # printed above; stopping here is what keeps a real error from being buried
  # under an endless retry loop.
  echo "[UPLOAD] NOT ALL OK - this does not look like a sleeping board." >&2
  echo "[UPLOAD] The port $PORT still exists and the error does not match any" >&2
  echo "[UPLOAD] known device-presence failure, so it is treated as a real" >&2
  echo "[UPLOAD] build, configuration, or tooling error and NOT retried." >&2
  echo "[UPLOAD] The complete upload output is printed above." >&2
  exit "$upload_status"
done
