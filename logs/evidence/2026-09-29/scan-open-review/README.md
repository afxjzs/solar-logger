# Scan-open diagnostic fix — review evidence, 2026-09-29

Evidence for the open-for-scan correctness fix committed as **`44d8b27`**,
"Report an unreadable log instead of claiming an intact tail". Narrative and
interpretation live in the newest [LAB_NOTES](../../../LAB_NOTES.md) entry and
in [BACKLOG](../../../BACKLOG.md); this directory holds the raw artifacts.

**No board was touched to produce any of it.** Nothing was uploaded, no serial
port was opened, and Experiment 3 was not reset, cleared or read. Every failure
staged here is a software failure injection in a throwaway directory.

## Provenance

Two stages produced these files, on the same day and against the same bytes.

The **CODE stage** produced `candidate.diff`, `code-report.txt`,
`review-tests.txt`, `review-tests.json` and `source-sha256.json` while the fix
was still uncommitted.

The **orchestrator close** produced `redcheck.sh` and `redcheck-prefix.txt`,
then committed the four files as `44d8b27`.

`source-sha256.json` records the four reviewed source files. Those four hashes
were recomputed at commit time and **match the committed bytes exactly**, so
the artifacts here describe what actually landed:

| File | SHA-256 |
| --- | --- |
| `Arduino/solar-logger/storage.h` | `e86fa285c4af7909d3c850cf6b0fc25177b2cd1e62e805048d8b5eb58a6b571b` |
| `Arduino/solar-logger/storage.cpp` | `ea49858497a7ab41fcb526b17f30de9296e75b74de61c81baa5fd042fd0cecd7` |
| `Arduino/solar-logger/solar-logger.ino` | `b8b9368fc821aa25f30a70357067e8c88504ba28140dbf5bcff003fe1366d6c1` |
| `tests/test_characterization_storage_recovery.py` | `a0ccd0979f25e9f09360d7a08f819bde716e866a356d1e5f7015ca2ad42ea9dd` |

## Contents

| File | What it is |
| --- | --- |
| `candidate.diff` | The reviewed change, before commit |
| `code-report.txt` | The CODE stage's own report, as delivered |
| `review-tests.txt` | Targeted pytest run over the storage-recovery and protocol characterization files: **104 passed** |
| `review-tests.json` | argv, start/finish timestamps and exit status (**0**) for that run |
| `source-sha256.json` | SHA-256 of the four reviewed source files |
| `redcheck.sh` | Reproducible red-check: do the seven new tests fail against pre-fix `1980d94`? |
| `redcheck-prefix.txt` | That script's raw output: **5 failed, 2 passed** |

## The red-check, and why its staging is not a plain checkout

`redcheck-prefix.txt` answers whether the seven new tests actually detect the
defect. Checking out `1980d94` and running them does **not** answer it: the test
file's `reportScan()` reads `scan.openFailed`, which that commit's `AutoLogScan`
has no member for, so the host harness fails to compile and all seven ERROR at
fixture setup. Erroring is not failing, and would show only that the tests
cannot build.

So `redcheck.sh` stages the pre-fix tree with the `openFailed` field
**declared and never set**. `AutoLogScan scan = {}` value-initializes it to
false, making the staging deterministic while the behavior under test stays
exactly pre-fix.

Result: **5 failed, 2 passed**. The 2 that pass are the conservative half that
was already true before the fix — the log file is left untouched and the D-023
NVS reservation floor is applied.

The run also reproduced the second defect verbatim, driving
`autoStorageTruncateToValid()` directly against a log the scan never opened:

```text
[STORAGE] ERROR: Could not open the log for scanning.
[STORAGE] Rewriting the log to its valid prefix: 0 of 0 bytes.
[STORAGE] ERROR: Could not open the log for recovery.
```

The rewrite announced itself and proceeded. Only its own source open failing,
for the same reason the scan's had, stopped it from renaming an empty file over
the log.

### Reproducing it

```bash
logs/evidence/2026-09-29/scan-open-review/redcheck.sh /tmp/redcheck /tmp/redcheck.txt
```

It creates a detached worktree, removes it on exit, and never writes to the
repository working tree. pytest exit **1** is the expected result.

## Limits

- **Nothing here is a hardware result.** The fix is committed and has never run
  on a board. The uploaded image is still `1980d94`, which HAS the defect.
- A physical open failure has never been observed on this board. The harness
  produces one by refusing the read-open while leaving the file untouched, which
  does not establish what a real LittleFS fault does.
- The damaged-log branches remain host and synthetic tested only.
- `redcheck-prefix.txt` records pytest's own output. The tally is readable from
  the `FF..FFF` progress line and the five `FAILED` lines.

## File checksums

SHA-256 of every file in this directory except this README:

```text
ffd5226d938253526dc5978dbffc94a977b6b45745c323565193af7de9cf82b6  candidate.diff
884cb62d5ca8ca3d0ca7b7b7413c3596afd0ab0902c105979baedefe45a31efe  code-report.txt
61fdc726fe0acc3f36e8af9dc7deddb79cf1be752eca9cd86b14be71b6540844  review-tests.json
a4195c5e7b1cd3cf8245ff52175a9beabe0b85262837e21dc3f91e1e431bebff  review-tests.txt
8bcd6f6331b3b26e39adf8107d9561b1dde04a5dcefa279d0f226930b56af8f8  source-sha256.json
e674411e5a3611bc6b60264ed9078e13cf196057fdd681ee9ac447988c6ede0b  redcheck.sh
11a9a8b5e84f1ff116681d11256392c5a404ddeca12c143404c0aba9cdd1367a  redcheck-prefix.txt
```
