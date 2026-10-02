# CODE kickoff — storage scan-open diagnostic, 2026-09-29

Prepared for a FRESH dedicated CODE session. The orchestrator handles coordination,
review and hardware acceptance; CODE performs this implementation. No prior chat
context is required. This file alone does not claim an agent has started; receipt
of the user's kickoff assigns this bounded work. Recheck actual concurrent edits,
and ask only if there is a concrete ownership conflict or missing decision.

Work in `/Users/afxjzs/dev/projects/solar-charger`. Read AGENTS.md's canonical
order, then docs/CURRENT_STATE.md. Recheck Git and ownership. HEAD was 1980d94;
its healthy-log hardware smoke passed on September 29. Experiment 3 is active.

## One bounded correctness task

Fix the existing-log open-for-scan failure documented in BACKLOG. In
`autoStorageScan()` (`Arduino/solar-logger/storage.cpp`), a failed read-open
prints an error but returns a zero-initialized result without `readError`.
Recovery/INFO can consequently claim INTACT for a log they never read.

Propagate an explicit unreadable result. Preserve the file and prevent automatic
repair on this outcome; retain D-023's NVS reservation floor. Keep an absent log
and a readable empty log distinct from an existing unreadable log. Inspect the
caller output and completion semantics rather than assuming that setting one
field fixes every misleading success. Report any broader protocol issue before
expanding scope. Preserve record bytes/version, sequence policy, and healthy-log
behavior. Do not combine this with the separate generic STATUS-zero defect or
another module extraction.

Extend the existing file-backed storage characterization harness with a
controlled read-open failure. Execute the real firmware functions; show the
regression fails against the original behavior. Verify explicit unreadable
status/no INTACT claim, unchanged file bytes, no repair and correct sequence
floor; retain meaningful missing/empty/healthy coverage. Avoid redundant tests.

Use uv (installed under /Users/afxjzs/.local/bin). Run the canonical software
gate before/after the change as required by project documentation. Record actual
results and failures. If concurrent host work changes gate inputs, coordinate or
use an isolated snapshot and state the tested scope; never label a mixed snapshot
as the isolated firmware result.

## Ownership and preservation

CODE owns this firmware fix and its focused tests. Leave the uncommitted host
report/test, pandas dependency changes and prior evidence untouched. Coordinate
shared documentation edits with the parent/operator; update CURRENT_STATE,
LAB_NOTES and the relevant BACKLOG/design status with narrowly scoped edits.
Follow the documented firmware identity policy; do not present unuploaded code
as hardware validated. Existing dirty docs contain multiple unrelated stages.

No upload or serial/device commands. No reset, storage clear, erase or live
corruption injection. Synthetic failures belong only in disposable host fixtures.
Do not alter the sealed September 29 hardware evidence. Do not commit, stage all,
or push as a side effect: return a focused, reviewable diff, exact validation
scope/results, changed-file list, remaining risks and later hardware acceptance
needs. Software-only failure injection does not prove physical read-error behavior.
