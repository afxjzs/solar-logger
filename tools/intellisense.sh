#!/usr/bin/env bash

# Generate the VS Code C/C++ IntelliSense configuration for the firmware.
#
#   tools/intellisense.sh            regenerate the compilation database
#   tools/intellisense.sh --check    report staleness only, change nothing
#
# Arduino CLI produces the compilation database; tools/intellisense.py turns it
# into something cpptools can apply to the .ino that is open in the editor. The
# reasoning behind every transformation is documented at the top of that file.
#
# Nothing here hardcodes a path under ~/Library/Arduino15. Every include path,
# define, and flag comes from the database Arduino CLI just produced.
#
# WHEN TO RUN THIS
#
#   After adding or removing a function in the sketch  - the generated forward
#   prototypes change.
#
#   After adding or removing an #include               - Arduino CLI's library
#   detection changes the -I set.
#
#   `--check` answers "is it stale?" in well under a second and never touches
#   the database.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FQBN="esp32:esp32:XIAO_ESP32C3"
SKETCH_DIR="$REPO_ROOT/Arduino/solar-logger"

# Deliberately NOT under .vscode/. cpptools excludes '**/.vscode' from its own
# file operations by default (C_Cpp.files.exclude), and the previous location
# also put a 250 KB generated near-duplicate of the sketch inside the workspace
# where the tag parser could index it alongside the real source.
BUILD_DIR="$REPO_ROOT/build/intellisense"

LEGACY_BUILD_DIR="$REPO_ROOT/.vscode/arduino-build"

PYTHON="$REPO_ROOT/.venv/bin/python"

if [[ ! -x "$PYTHON" ]]; then
  PYTHON="python3"
fi

if [[ "${1:-}" == "--check" ]]; then
  exec "$PYTHON" "$REPO_ROOT/tools/intellisense.py" \
    --check \
    --build-path "$BUILD_DIR" \
    --sketch "$SKETCH_DIR"
fi

if [[ $# -gt 0 ]]; then
  echo "[INTELLISENSE] ERROR: Unknown argument: $1" >&2
  echo "[INTELLISENSE] Usage: tools/intellisense.sh [--check]" >&2
  exit 1
fi

if [[ -d "$LEGACY_BUILD_DIR" ]]; then
  # Said out loud rather than cleaned up silently. A leftover database under
  # .vscode/ is exactly the kind of thing an editor could still be reading.
  echo "[INTELLISENSE] NOTICE: The old build directory still exists:"
  echo "[INTELLISENSE]   $LEGACY_BUILD_DIR"
  echo "[INTELLISENSE] Nothing uses it any more. Remove it when convenient:"
  echo "[INTELLISENSE]   rm -rf $LEGACY_BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"

echo "[INTELLISENSE] Generating Arduino CLI compilation database..."
echo "[INTELLISENSE] Target: $FQBN"
echo "[INTELLISENSE] Build path: $BUILD_DIR"

arduino-cli compile \
  --only-compilation-database \
  --fqbn "$FQBN" \
  --build-path "$BUILD_DIR" \
  "$SKETCH_DIR"

exec "$PYTHON" "$REPO_ROOT/tools/intellisense.py" \
  --build-path "$BUILD_DIR" \
  --sketch "$SKETCH_DIR"
