# Storage extraction review — 2026-09-29

Local software evidence; no upload or device command was run. These files are
part of the isolated storage commit based on 43c3baf. Experiment 3 remains intact
by non-interference; this review did not make a new board observation.

- [storage-move-audit.txt](storage-move-audit.txt): fresh comparisons using the
  repository's firmware_source token reader. Four moved bodies and AutoLogScan
  match the parent; recovery halves rejoin exactly. The adapted commands and wake
  body match after expanding accessors. See the September 29 LAB_NOTES entry.
- [storage-only-gate.txt](storage-only-gate.txt): complete tools/check.sh output
  from a temporary Git snapshot of 43c3baf plus the four storage source/test files.
  Exit 0, ALL OK, 462 passed/1 xfailed, clean types and warning-free firmware
  compile, 7/7 editor units, shell/whitespace pass. The 24 capture-gap tests and
  pandas additions are absent from this snapshot. No source or lockfile changed
  during validation; code was compared byte-for-byte before staging.
- [SHA256SUMS](SHA256SUMS): SHA-256 checksums of the two raw output files.

Flash 1,104,313 B; globals 36,332 B. These are uninjected compile results, not a
hardware image identity. The full combined tree's September 28 result remains
486/1; that is different scope, not a regression. Hardware smoke is still owed.
The temporary snapshot path printed in the log is provenance, not a dependency
for future work. A pre-existing open-for-scan diagnostic defect was recorded in
LAB_NOTES/BACKLOG and not mixed into this structural commit.
