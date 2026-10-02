# Pre-upload baseline — recovery smoke for 16a299d, 2026-10-01

Captured before any device access. No board was touched to produce this file.

## What is under test

Two commits, neither of which has ever run on hardware. The board is on
`58a4e09`, accepted earlier the same day.

| Commit | Change |
| --- | --- |
| `6192c2d` | D-061: recovery is told whether the retained RTC state was valid, and zeroes or carries the running totals accordingly |
| `16a299d` | D-062: the D-023 sequence decision extracted into `sequence_authority`, a pure function |

Both touch the recovery path. Both changed behavior only on branches a healthy
log never takes, so this is a regression check on the path that must not have
moved.

## Git

```text
16a299d Extract the D-023 sequence decision into its own module
7338fd4 Remove the evidence files left behind at the old path
6192c2d Tell recovery whether the retained totals were ever valid
b3b10ab Preserve hardware acceptance evidence and its tooling under logs/
58a4e09 Keep the retained running totals when the log was not read
```

HEAD `16a299d` on main, nothing pushed. Documentation remains uncommitted and
spans several stages.

## Source SHA-256, as built for this upload

```text
0fa15ed8968dd6ad70ff1019c1f1d6903654a04336bf2fda81d25105fc49a585  Arduino/solar-logger/solar-logger.ino
4ffea3f842f1802db1e1357124d4e6d112df53cd20dd9dd6104316807e1cf953  Arduino/solar-logger/sequence_authority.cpp
202aa6312755e768563b069fdc0f6fed1592bc13405b7f497afa4b62baba8ec5  Arduino/solar-logger/sequence_authority.h
3d6ae67bf77c5b53c52b16b85d6126a83e1f0ec101c1cce398c35a6bcca3ad3b  Arduino/solar-logger/storage.cpp
908b0b593637a5d4878fc0a7abc0337430ed8a1cf77bc2789af7e91ce28cee20  Arduino/solar-logger/storage.h
```

## Software gate at this tree

`tools/check.sh` exit 0, `ALL OK`, all seven steps. pytest **529 passed,
1 xfailed**. Flash **1,106,273 B** (84%), globals **36,332 B** (11%),
IntelliSense **8/8**. Sketch 6,680 lines. This is the uninjected gate build;
`tools/upload.sh` injects the Git revision, and the previous two uploads each
came out 48 bytes smaller than their gate build.

## Acceptance criteria

**Positive:**

- `VERSION` reports `0.3.0-dev / 16a299d / solar-logger-protocol-ack-v3`.
- Both `LOGGER STORAGE INFO` scans: `Tail status: INTACT`, `Trailing bytes: 0`.
- Final dump: board invalid 0, all `exp=3`, complete BEGIN/END, summary, ACK and
  matching `RESULT OK`; every preflight record line byte-identical.
- **D-060/D-061:** the running totals continue across the upload's reset.
- **D-062:** the sequence continues across the upload's reset with NO gap.

That last one is the only hardware-observable consequence of D-062. `LOGGER
STORAGE INFO` does not run the sequence decision, and the recovery transcript
happens at a cold boot the sender does not capture. But on a healthy log D-023
makes the log the authority, so the next sequence must be `lastSeq + 1`. A jump
would mean the NVS reservation floor was applied to a log the board itself
reported INTACT. `tools/check-totals-continuity.py` asserts it at the boundary
identified from the preflight dump.

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
them appearing means the healthy path moved.

## Planned immediately after acceptance

Raise the logging cadence. The log fills around 2026-10-05 at 60 seconds, and
`LOGGER INTERVAL` is refused while armed, so this needs `AUTONOMOUS OFF`, set,
`AUTONOMOUS ON`. That clears `rtcAutoMagic` and re-arms, which is exactly the
path D-061 changed — which is why it is sequenced AFTER this acceptance rather
than before it. Doing it first would exercise unvalidated code.

## Limits

- Nothing here is a hardware result until the captures exist.
- Failure branches remain host-injected only; this says nothing about a physical
  LittleFS fault.
- Board-reported CRC validity is not independent binary verification.
- All epochs remain 0 and time `UNKNOWN`.
