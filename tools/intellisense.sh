#!/usr/bin/env bash

# Generate the VS Code C/C++ IntelliSense configuration for the firmware.
#
#   tools/intellisense.sh            regenerate and verify the database
#   tools/intellisense.sh --check    report staleness only, change nothing
#
# Arduino CLI produces a compilation database; tools/intellisense.py turns it
# into one cpptools can apply to every project file a person edits: the .ino
# and each .c/.cpp module beside it, found automatically. The reasoning behind
# every transformation is documented at the top of that file.
#
# Nothing here hardcodes a path under ~/Library/Arduino15, and nothing names a
# module. Every include path, define, and flag comes from the database Arduino
# CLI just produced.
#
# WHEN TO RUN THIS
#
#   tools/check.sh runs it on every check, so adding a module and running the
#   check is enough. Run it directly after adding a module, an #include, or a
#   function that is called above where it is defined, if you want the editor
#   updated before the next check.
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
#
# The editor reads $OUTPUT_DIR/compile_commands.json. Arduino CLI builds into
# $OUTPUT_DIR/arduino-cli/, so its raw database never overwrites the editor's.
OUTPUT_DIR="$REPO_ROOT/build/intellisense"

LEGACY_BUILD_DIR="$REPO_ROOT/.vscode/arduino-build"

PYTHON="$REPO_ROOT/.venv/bin/python"

if [[ ! -x "$PYTHON" ]]; then
  PYTHON="python3"
fi

if [[ "${1:-}" == "--check" ]]; then
  exec "$PYTHON" "$REPO_ROOT/tools/intellisense.py" \
    --check \
    --sketch "$SKETCH_DIR" \
    --output-dir "$OUTPUT_DIR"
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

if [[ -d "$OUTPUT_DIR/sketch" ]]; then
  # Before 2026-09-18 Arduino CLI built straight into $OUTPUT_DIR. Its output
  # is not read any more, and is left alone rather than deleted unasked.
  echo "[INTELLISENSE] NOTICE: $OUTPUT_DIR holds Arduino CLI output from the old layout"
  echo "[INTELLISENSE] (sketch/, core/, libraries/, intellisense/ and the files beside"
  echo "[INTELLISENSE] them). Nothing reads it now; Arduino CLI builds into arduino-cli/."
  echo "[INTELLISENSE] To clear it: rm -rf $OUTPUT_DIR, then run this script again."
fi

exec "$PYTHON" "$REPO_ROOT/tools/intellisense.py" \
  --sketch "$SKETCH_DIR" \
  --output-dir "$OUTPUT_DIR" \
  --fqbn "$FQBN"
