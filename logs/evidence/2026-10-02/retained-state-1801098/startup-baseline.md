# Pre-upload baseline — retained-state acceptance for `1801098`, 2026-10-02

Written before the upload and before any post-upload capture, so the acceptance
criteria below are stated without knowing the result.

**Device access before this file existed:** two read-only captures,
`probe-version` and `probe-storage-info`, both taken during ordinary 10-second
rendezvous windows. Nothing was changed on the board. This file is not claiming
"no board was touched"; it is claiming that no criterion below was written after
seeing an outcome it judges.

Those two were originally labeled `pre-version` and `pre-storage-info` and were
**renamed to `probe-*` before the driven stages ran**, because
`tools/storage-smoke.py preflight` owns those two labels and
`capture-command.py` correctly refuses to overwrite a capture. The `.txt` bytes
are untouched and each `.json` still carries the SHA-256 of its own `.txt`; the
label is not stored inside the JSON, so only the filenames changed. Recorded
rather than done quietly.

## What is under test

One commit, which has never run on hardware.

| Commit | Change |
| --- | --- |
| `1801098` | D-063: the ten `rtcAuto*` definitions and `AUTO_RTC_MAGIC` moved into `autonomous_retained_state.{h,cpp}`, exported by `extern`. No writer moved; no transition moved; reservation and the power-test RTC pair stayed in the sketch |

**This is a pure relocation of definitions, and the linked image proves it.**
All ten allocated `PROGBITS` sections keep their address and size, exactly four
bytes differ across all of them (the core's compile-time stamp in
`.flash.rodata`), and the two symbol tables diff empty. All twelve RTC symbols
keep their addresses: `0x50000018`–`0x5000004F`, `rtcSleepTest*` last.

So this acceptance is **mostly a check of the build and upload path**, not of
new code paths. Its value is in the one risk a relocation does carry: a
`RTC_DATA_ATTR` definition that ended up in more than one translation unit, or
in a header, would give units their own silent copies and the autonomous session
clock would reset without a word. BACKLOG calls that the single highest-risk
detail in the whole modularization plan. Steps 7 and 8 below are the ones that
would catch it.

## Git

```text
8faa0e3 Add the offline charger-transition and capture-gap report
1801098 Extract the RTC-retained autonomous state into its own module
65f55d1 Clear documentation drift and rebuild the checkpoint
```

HEAD `8faa0e3` on main, **pushed to `origin/main`**. Working tree clean.

**The image will report revision `8faa0e3`, not `1801098`.** `tools/upload.sh`
stamps `git rev-parse --short HEAD`, and `8faa0e3` changed no firmware file — it
added `tools/charger-transitions.py`, its tests and the pandas dependency. The
firmware content being uploaded is `1801098`'s. Recorded here so the captures
are not read as the wrong image.

## Source SHA-256, as built for this upload

```text
89bc396d73b606ddd1b041498755e8246cb5350010e774c77d0dca5168376762  Arduino/solar-logger/solar-logger.ino
e9ee1d2e663a94eb879bb6e7edccced3341fe19a2997a7f14bb23474a915b892  Arduino/solar-logger/autonomous_retained_state.h
195f7ce25aa9212c8db56d2a8ff7b0d26e32ef18c1887be17157e092ec0ee64b  Arduino/solar-logger/autonomous_retained_state.cpp
4a581f8c8e474cc838e6a4235fbe0e7d2770a0913d5e17a2a7a8850e6eeb32f5  Arduino/solar-logger/sequence_authority.h
```

## Software gate at this tree

`tools/check.sh` exit 0, `ALL OK`, all seven steps. pytest **563 passed,
1 xfailed**. Flash **1,106,273 B** (84%), globals **36,332 B** (11%),
IntelliSense **9/9**. Sketch 6,670 lines. Run twice: by the implementing CODE
stage and independently by the orchestrator. This is the uninjected gate build;
`tools/upload.sh` injects the Git revision, and the previous uploads each came
out 48 bytes smaller than their gate build.

## Board state before the upload

From `probe-storage-info`, this date:

```text
[STORAGE] Valid records:    15089
[STORAGE] Oldest sequence:  1412
[STORAGE] Newest sequence:  16500
[STORAGE] Trailing bytes:   0
[STORAGE] Tail status:      INTACT
[STORAGE] Room for 4664 more records = 16 days at 300 second cadence
```

`probe-version` reports `0.3.0-dev / 16a299d / solar-logger-protocol-ack-v3`.

## Acceptance criteria

**Positive:**

- `VERSION` reports `0.3.0-dev / 8faa0e3 / solar-logger-protocol-ack-v3`.
- Both `LOGGER STORAGE INFO` scans: `Tail status: INTACT`, `Trailing bytes: 0`.
- Final dump: board invalid 0, all `exp=3`, complete BEGIN/END, summary, ACK and
  matching `RESULT OK`; every preflight record line byte-identical.
- The running totals continue across the upload's reset (D-060, D-061).
- The sequence continues across the upload's reset with **no gap** (D-023,
  D-062). `tools/check-totals-continuity.py` asserts both at the boundary
  identified from the preflight dump.

**Retained-state criteria specific to D-063.** These are the reason the full
2026-09-17 sequence is required rather than the short extraction smoke:

- **Step 7, lease expiry.** Hold a session and stop sending keepalives. Expect
  expiry near 15 s with no multi-second Serial stall, and a return to autonomous
  operation. The record after it must continue `session_elapsed_ms` rather than
  restarting near zero.
- **Step 8, five consecutive unclaimed records.** `boot_id` identical across all
  five, `[AUTO] Cycle:` increasing by exactly one per wake, record sequences
  contiguous, `session_elapsed_ms` never decreasing while `boot_id` is unchanged
  (D-033), and each `interval_ms` near 300,000 ms.
- `LOGGER AUTONOMOUS STATUS` during a rendezvous: `[AUTO] RTC state valid: YES`,
  with `Cycles this power-on:` risen since the upload's cold boot.

**A duplicated RTC definition would present as:** `[AUTO] WARNING: Timer wake
with no valid RTC session state.` on every timer wake, a fresh `boot_id` each
wake, and cycle count stuck at 0. None of those may appear.

**Negative** — none of these may appear in any capture:

```text
UNREADABLE
could not be opened
Running totals could NOT be recovered
Carrying the retained totals instead
Retained RTC state was
Restarting the running totals
was never read
[STORAGE] ERROR
[STORAGE] WARNING
```

Each is printed only by a branch an unhealthy log takes. On a healthy log one of
them appearing means the healthy path moved. `tools/storage-smoke.py` asserts
their absence on every capture rather than once at the end.

## Deliberate deviation from the 2026-09-17 sequence

**Step 4 is not run as written, and step 3 is run differently.** The sequence
says `LOGGER INTERVAL 5` (expect `ERROR`, exit 1) then `LOGGER INTERVAL 60`
(expect `OK`). Step 4 would revert the 2026-10-02 cadence decision and cut
storage headroom from about 16 days to about 3. `LOGGER INTERVAL` is also
refused while armed, so it would need a disarm and re-arm, which clears
`rtcAutoMagic` — the exact path this acceptance exists to validate, exercised
before it has been validated.

Substituted: `LOGGER INTERVAL 5` is sent while **armed**, where it is refused
and changes nothing, which still demonstrates `CMD_RESULT,...,ERROR` and exit 1.
"A valid command still returns `OK`" is covered by `VERSION`,
`LOGGER STORAGE INFO`, `HOLD`, `KEEPALIVE` and `RELEASE`, all of which are
captured. **The cadence stays at 300 seconds.** It must read 300 in the final
`LOGGER AUTONOMOUS STATUS`.

`capture-command.py` refuses to pass `INTERVAL` to `send.sh` at all, by design,
so that one step is sent directly and its output pasted into the record rather
than captured with a checksum. That is a weaker form of evidence and is labeled
as such where it appears.

## Limits

- Nothing here is a hardware result until the captures exist.
- Failure branches remain host-injected only; this says nothing about a physical
  LittleFS fault, a failed open or a short read.
- Board-reported CRC validity is not independent binary verification.
- All epochs remain 0 and time `UNKNOWN`. The durable log cannot be dated from
  its own contents; `data/samples.csv` is the only wall-clock reference.
- The image is byte-identical to the pre-move build apart from the build stamp,
  so a pass here does not exercise any new instruction. It exercises the build,
  the upload, and the single-definition property of the moved RTC state.
- No reset, storage clear, whole-flash erase or corruption injection is
  performed. Experiment 3 is not reset at any point.
