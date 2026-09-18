#!/usr/bin/env bash

# Local validation gate. Run it before and after every modularization stage.
# See docs/DECISIONS.md D-043.
#
#   tools/check.sh
#
# Steps:
#
#   pytest            uv run pytest
#   pyright           uvx pyright against the project venv, every Python file
#   py_compile        every Python file
#   firmware compile  arduino-cli compile --clean --warnings all; any warning FAILS
#   shell syntax      bash -n on every tools/*.sh
#   whitespace        git diff HEAD --check (tracked files only)
#
# Every step runs even after one fails, so a single run shows everything that
# is wrong. The summary is built from each step's real exit status, never from
# what was expected, and the script exits 1 if any step failed.
#
# It uploads nothing, opens no serial port, and touches no device. The compile
# builds into arduino-cli's own cache, not the repository. It is NOT the image
# tools/upload.sh would flash: that one carries an injected Git revision, and
# this one would print "Revision: UNKNOWN".

set -u

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

FQBN="esp32:esp32:XIAO_ESP32C3"
SKETCH_DIR="Arduino/solar-logger"

shopt -s nullglob
PYTHON_FILES=(app/*.py tools/*.py tests/*.py main.py)
SHELL_FILES=(tools/*.sh)

RESULTS=()
ANY_FAILED=0

record() {
  local name="$1"
  local status="$2"

  if (( status == 0 )); then
    RESULTS+=("PASS  $name")
  else
    RESULTS+=("FAIL  $name (exit $status)")
    ANY_FAILED=1
  fi
}

run_step() {
  local name="$1"
  shift

  echo
  echo "============================================================"
  echo "[CHECK] $name"
  echo "============================================================"

  "$@"
  record "$name" "$?"
}

compile_firmware() {
  local output
  local status

  # --clean is required, not tidiness. Without it arduino-cli reuses cached
  # objects for unchanged source and prints no warnings for them, so a warning
  # fails the gate once and then passes silently on every later run. Observed
  # while building this script on 2026-09-18.
  echo "[CHECK] arduino-cli compile --clean --warnings all --fqbn $FQBN $SKETCH_DIR"
  output="$(arduino-cli compile --clean --warnings all --fqbn "$FQBN" "$SKETCH_DIR" 2>&1)"
  status=$?
  printf '%s\n' "$output"

  if (( status != 0 )); then
    return "$status"
  fi

  # --warnings all prints warnings and still exits 0. The recorded state of this
  # firmware is "no warnings", so a new one fails the gate instead of scrolling
  # past in a successful build.
  if grep -qi "warning:" <<< "$output"; then
    echo "[CHECK] ERROR: the compile succeeded but emitted warnings (above)."
    return 1
  fi

  return 0
}

check_shell_syntax() {
  local file
  local status=0

  for file in "${SHELL_FILES[@]}"; do
    if bash -n "$file"; then
      echo "[CHECK] bash -n $file: OK"
    else
      echo "[CHECK] bash -n $file: FAILED"
      status=1
    fi
  done

  return "$status"
}

check_whitespace() {
  local untracked
  local status

  git --no-pager diff HEAD --check
  status=$?

  # Said out loud, because a clean result here says nothing about them.
  untracked="$(git ls-files --others --exclude-standard)"

  if [[ -n "$untracked" ]]; then
    echo "[CHECK] NOTE: untracked files are not covered by git diff --check:"
    printf '%s\n' "$untracked" | sed 's/^/[CHECK]   untracked: /'
  fi

  return "$status"
}

run_step "pytest" uv run pytest
run_step "pyright" uvx pyright --pythonpath .venv/bin/python "${PYTHON_FILES[@]}"
run_step "py_compile" uv run python -m py_compile "${PYTHON_FILES[@]}"
run_step "firmware compile" compile_firmware
run_step "shell syntax" check_shell_syntax
run_step "whitespace" check_whitespace

echo
echo "============================================================"
echo "[CHECK] SUMMARY"
echo "============================================================"

for line in "${RESULTS[@]}"; do
  echo "[CHECK]   $line"
done

if (( ANY_FAILED )); then
  echo "[CHECK] NOT ALL OK"
  exit 1
fi

echo "[CHECK] ALL OK"
