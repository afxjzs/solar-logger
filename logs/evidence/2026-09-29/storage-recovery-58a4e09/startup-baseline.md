# Pre-upload baseline — storage recovery smoke for 58a4e09, 2026-09-29

Captured before any device access. No board was touched to produce this file.

## What is under test

Two commits, neither of which has ever run on hardware:

| Commit | Change |
| --- | --- |
| `44d8b27` | A log that exists but will not open is reported UNREADABLE instead of INTACT, and `LOGGER STORAGE INFO` answers ERROR |
| `58a4e09` | Recovery keeps the retained running totals, and says so, when the scan could not read the log (D-060) |

Both changed storage recovery **only on paths a healthy log never takes**. So
this smoke is a regression check on the path they must not have moved, not a
test of the new branches. The new branches need a real filesystem fault and
**must not** be obtained by damaging the Experiment 3 log.

## Git

```text
58a4e09 Keep the retained running totals when the log was not read
44d8b27 Report an unreadable log instead of claiming an intact tail
1980d94 Extract durable storage mechanics
43c3baf Fix durable log recovery and reconcile project docs
```

HEAD `58a4e09` on main, three ahead of origin/main, nothing pushed. Firmware and
test files are clean; documentation and the host capture-gap stage remain
uncommitted and were not part of either commit.

## Source and tool SHA-256, as built for this upload

```text
1e4de35b960740b750e6ec67bdc86e25be8cd2c315bd7ab42ca58d9ce30ad11a  Arduino/solar-logger/solar-logger.ino
ea49858497a7ab41fcb526b17f30de9296e75b74de61c81baa5fd042fd0cecd7  Arduino/solar-logger/storage.cpp
e86fa285c4af7909d3c850cf6b0fc25177b2cd1e62e805048d8b5eb58a6b571b  Arduino/solar-logger/storage.h
d303fa7ba88c1f2da5dd2dc974449dc007e9a212af2154234a44a476ba610726  Arduino/solar-logger/record_format.cpp
a298961b451fb2a978fe07f528c4680493a8a2ff700ffa13ea1a977528699383  tools/upload.sh
aaeb1e73ab68f13c9e8f64e252fe6d0a690433856e9707e4dbb396e3b1f09a97  tools/send.sh
```

`storage.cpp` and `storage.h` are byte-identical to the `44d8b27` review
manifest, so nothing moved in the storage module between the two commits.

## Software gate at this tree

`tools/check.sh` exit 0, `ALL OK`, all seven steps PASS. pytest **496 passed,
1 xfailed**. Flash **1,105,707 B** (84%), globals **36,332 B** (11%),
IntelliSense 7/7. This is the uninjected gate build; `tools/upload.sh` injects
the Git revision, so the uploaded image's size will differ from that figure.

## Acceptance criteria

**Positive** — all must hold:

- `VERSION` reports `0.3.0-dev / 58a4e09 / solar-logger-protocol-ack-v3`.
- Both `LOGGER STORAGE INFO` scans: `Tail status: INTACT`, `Trailing bytes: 0`,
  no storage error.
- Final dump: board invalid 0, all `exp=3`, complete BEGIN/END, count summary,
  ACK and matching `RESULT OK`.
- Every preflight decoded record line is byte-identical in the final dump.
- **The running totals continue across the upload's reset.** For the first
  record of the new boot, `Qsum_uAh` equals the previous record's `Qsum_uAh`
  plus its own `dQ_uAh`, and likewise for `Esum_uWh`. This is the D-060
  healthy-path check and is verified by `tools/check-totals-continuity.py`.

**Negative** — none of these may appear anywhere in any capture:

```text
UNREADABLE
could not be opened
Running totals could NOT be recovered
Carrying the retained totals instead
was never read
[STORAGE] ERROR
[STORAGE] WARNING
```

Each is a string only the new failure branches can print. On a healthy log, one
of them appearing means the healthy path moved. `tools/storage-smoke.py` asserts
their absence on every command and stops acceptance if one is found.

## Where the procedure lives

The capture, validation and smoke-driving scripts are in `tools/`, not in this
directory, and they take the evidence directory as an argument:

| Tool | Role |
| --- | --- |
| `tools/capture-command.py` | runs one canonical command, preserves output plus argv/times/exit/SHA-256 |
| `tools/storage-smoke.py` | drives a stage and asserts its result, including the negative criterion |
| `tools/validate-dump.py` | validates a dump and compares it to an earlier one |
| `tools/check-totals-continuity.py` | the D-060 reseed arithmetic |

The earlier `storage-hardware/` evidence carries its own copies of the first
three, from before they were tools. Those copies are sealed evidence of that
run and are deliberately left alone rather than retrofitted. This directory
holds only outputs.

All four were re-validated against that earlier run's dumps after the move and
reproduce its numbers exactly: 10,664 records, 10,659 identical historical
record lines, zero sequence gaps, and the reseed check passing at boot 35 -> 36.

## Prior measured baseline, for comparison

From the `1980d94` smoke earlier the same day
([evidence](../storage-hardware/README.md)): final dump 10,664 records,
sequences 1412–12075, board invalid 0, all `exp=3`. Last record seq 12075,
boot 36, `Qsum_uAh=1012253`, `Esum_uWh=16147177`. The board has been logging
autonomously at 60 s since, so the preflight count here will be higher.

Running `check-totals-continuity.py` against that dump reports **zero**
continuity breaks across all 10,664 records, so the check is strict and has
always held on this log.

## Known diagnostics that are NOT failures

- Generic `STATUS` during an early timer wake prints uninitialized experiment
  zeros. NVS-backed `LOGGER AUTONOMOUS STATUS` and the records are authoritative.
  Tracked separately in BACKLOG.
- `HOLD`/`RELEASE` previously warned accumulator reset readback `ENERGY=0
  CHARGE=-1`. Cause unmeasured; not a storage error.
- All epochs remain 0 and time `UNKNOWN`; there is no wall clock.
- Board-reported CRC validity is not independent binary verification.
