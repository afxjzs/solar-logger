#!/usr/bin/env bash

# Red-check for the scan-open diagnostic fix (commit 44d8b27).
#
#   logs/evidence/2026-09-29/scan-open-review/redcheck.sh <worktree> <output>
#
# Asks one question: do the seven new scan-open tests actually fail against the
# code as it was BEFORE the fix? A test that passes either way proves nothing.
#
# WHY THE STAGING IS NOT JUST "CHECK OUT 1980d94". The new test file's
# reportScan() reads scan.openFailed, and 1980d94's AutoLogScan has no such
# member, so the host harness does not compile and all seven tests ERROR at
# fixture setup. Erroring is not failing: it would show the tests cannot build,
# not that they detect the defect. So this stages the pre-fix tree with ONLY the
# field DECLARED and never set anywhere. `AutoLogScan scan = {}` in
# autoStorageScan() value-initializes it to false, so the staging is
# deterministic and the behavior under test is exactly pre-fix.
#
# Expected: 5 failed, 2 passed. The 2 that pass are the conservative half that
# was already true before the fix - the file is left untouched and the D-023 NVS
# reservation floor is applied.
#
# It uploads nothing, opens no serial port and touches no device. It never
# writes to the repository working tree: the only tree it modifies is the
# throwaway worktree, which it removes on the way out.

set -u

REPO="/Users/afxjzs/dev/projects/solar-charger"
PREFIX_COMMIT="1980d94"

WORKTREE="${1:?usage: redcheck.sh <worktree-path> <output-file>}"
OUTPUT="${2:?usage: redcheck.sh <worktree-path> <output-file>}"

TESTS="tests/test_characterization_storage_recovery.py"
SELECT="cannot_be_opened or unopenable or three_answers or could_not_open"

cleanup() {
  git -C "$REPO" worktree remove --force "$WORKTREE" 2>&1 || true
}

if ! git -C "$REPO" worktree add --detach "$WORKTREE" "$PREFIX_COMMIT"; then
  echo "FAILED: could not create the worktree at $PREFIX_COMMIT" >&2
  exit 1
fi

trap cleanup EXIT

# `cp` is aliased to `cp -i` in the user's shell; `command` sidesteps the alias
# so this cannot stall waiting for a confirmation nobody is there to give.
if ! command cp -f "$REPO/$TESTS" "$WORKTREE/$TESTS"; then
  echo "FAILED: could not stage the new test file" >&2
  exit 1
fi

# Declare the field, and nothing else. Fails loudly rather than silently
# staging nothing if the anchor line ever changes.
if ! python3 - "$WORKTREE/Arduino/solar-logger/storage.h" <<'STAGE'
import sys

path = sys.argv[1]

with open(path, encoding="utf-8") as handle:
    text = handle.read()

anchor = "\tbool readError;                    // the log could not be read to the end\n"

if anchor not in text:
    sys.exit(f"anchor line not found in {path}; refusing to stage a guess")

added = anchor + "\tbool openFailed;                   // RED-CHECK ONLY: declared, never set\n"

with open(path, "w", encoding="utf-8") as handle:
    handle.write(text.replace(anchor, added, 1))
STAGE
then
  echo "FAILED: could not stage the openFailed declaration" >&2
  exit 1
fi

{
  echo "Red-check: the seven new scan-open tests against pre-fix $PREFIX_COMMIT"
  echo "Staging: new test file copied in; openFailed DECLARED in storage.h, never set."
  echo "Expected: 5 failed, 2 passed."
  echo
} > "$OUTPUT"

uv run --directory "$WORKTREE" pytest "$TESTS" \
  -k "$SELECT" --no-header -p no:cacheprovider -q >> "$OUTPUT" 2>&1
status=$?

echo >> "$OUTPUT"
echo "pytest exit status: $status (1 is expected here: these tests MUST fail pre-fix)" >> "$OUTPUT"

echo "Red-check output written to $OUTPUT (pytest exit $status)"
