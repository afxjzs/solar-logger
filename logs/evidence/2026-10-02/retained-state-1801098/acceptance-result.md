# Acceptance result — retained-state extraction `1801098`, 2026-10-02

**PASSED.** Every criterion in [startup-baseline.md](startup-baseline.md) was met.
Written after the captures existed; the criteria were written before them.

Board image: `0.3.0-dev / 8faa0e3 / solar-logger-protocol-ack-v3`. Experiment 3
intact and still logging at 300 seconds. No reset, storage clear, whole-flash
erase or corruption injection was performed at any point.

## The assertions

Both run with `--compare` against the preflight dump, and both exit statuses
read directly rather than through a pipe.

```text
tools/validate-dump.py post-dump.txt --compare pre-dump.txt --experiment 3   -> exit 0
tools/check-totals-continuity.py post-dump.txt --compare pre-dump.txt        -> exit 0
```

| Fact | Value |
| --- | --- |
| Records, post | **15,106** (15,091 before the upload) |
| Sequence range | 1412 – 16517 |
| Sequence gaps | **none** |
| Board invalid | **0** |
| Experiment ids | `3` only |
| Preflight record lines reproduced byte-identically | **15,091 of 15,091** |
| Other continuity breaks anywhere in the log | **none** |

The continuity boundary was **identified from the preflight dump, not inferred**.
That distinction matters: the tool's own docstring records that an inferred
boundary in the `58a4e09` acceptance landed on boot 39→40 while the upload was
36→37, producing "a result that looks like evidence and is not."

```text
last_before_reset   seq 16503  boot 42  Qsum_uAh 1264889  Esum_uWh 20696849
first_after_reset   seq 16504  boot 43  dQ 3214  dE 42883
                                        Qsum_uAh 1268103  Esum_uWh 20739732

1264889 + 3214 = 1268103          20696849 + 42883 = 20739732
sequence_contiguous: true
```

## The negative criterion

All nine forbidden strings from the baseline, plus
`no valid RTC session state`, are absent from **every** capture in this
directory. Each is printed only by a branch an unhealthy log or an invalid
retained state takes.

## What this established about the ten relocated variables

The extraction moved definitions only. The risk it carried was a definition
reaching more than one translation unit, which would give each unit a silent
copy. Nine of the ten are directly evidenced:

| Variable | Evidence |
| --- | --- |
| `rtcAutoMagic` | `RTC state valid: YES` at every status; no `no valid RTC session state` warning anywhere |
| `rtcAutoBootId` | 42 → 43 across the upload's reset, then **constant 44** across six consecutive wakes |
| `rtcAutoCycleCount` | **`FIRST_AFTER_BOOT` on exactly one record out of six.** Set when the counter is 0; per-unit copies would have set it on every wake |
| `rtcAutoSessionElapsedMs` | Monotonic within boot 44: 323,711 → 1,824,071 ms in ~300,060 ms steps (D-033) |
| `rtcAutoIntervalStartMs` | `interval_ms=300004` on all six boot-44 records |
| `rtcAutoNextSeq` | Contiguous 16512–16517, and continuous across the reset at 16503 → 16504 |
| `rtcAutoSeqHighWater` | **Both directions.** Read: the floor was declined twice in favor of an intact log (below). Write: the final status shows `reserved through 16580`, so a new block was persisted and the mirror advanced after the NVS write succeeded |
| `rtcAutoRunChargeUAh` | Arithmetic closes across the reset and across all five boot-44 transitions |
| `rtcAutoRunEnergyUWh` | Same |

**`rtcAutoCommandedSleepMs` is NOT directly evidenced, and this is a structural
limit rather than an oversight.** Its value is printed only in the wake report,
which is emitted before the rendezvous opens. No USB device exists at that
moment, and `Serial.setTxTimeoutMs(0)` (D-027) drops output rather than
buffering it, so neither `tools/send.sh` nor `app/solar_logger.py` can receive
that line — a host attaching at the rendezvous is structurally too late. Its
correctness is evidenced only indirectly, by the commanded sleeps producing the
right durations. Capturing it would need a host attached across a wake boundary,
which the current transport cannot provide.

## The sequence decision, observed twice with its inputs in contention

D-062 noted that only an arming transcript shows the decision, because the
cold-boot path runs the same code where no capture reaches it. This acceptance
caught it twice:

```text
post-identify (after the upload's cold boot, inferred from the result)
    pre-upload last record 16502, floor 16516, next used 16503/16504/16505
    -> the floor was 14 ahead and was not applied

post-step9-rearm (arming while tethered, so the transcript is visible)
    [STORAGE] Next sequence from log: 16512, from NVS reservation: 16517, using: 16512
    [STORAGE] Log tail is provably intact, so it is the authority and the NVS
              reservation floor was not applied.
    -> the floor was 5 ahead and was not applied
```

Had the floor been applied in either case, the log would carry a 14-number or
5-number hole that the board itself reports as INTACT.

## Two results beyond the stage under test

- **The first recorded hardware lease-expiry trace.** `post-lease-expiry-2`
  holds `Host lease EXPIRED after 15000 ms without keepalive`, the firmware
  releasing the USB transport itself, the open tethered interval closing, and
  the return to autonomous sleep. PROJECT.md has said lease-expiry timing was
  "covered by host tests, but no explicit expiry transcript was found," and
  LAB_NOTES has carried "an explicit lease-expiry trace remains outstanding"
  since 2026-09-11. Both can now be closed.
- **The D-046 handoff asymmetry, observed side by side.** `RELEASE` slept
  `299750 ms`, one cadence minus the 250 ms result-delivery grace; lease expiry
  slept `300000 ms`, a full cadence, because it has no result to deliver. D-046
  stated that difference and it had never been observed at two cadences. At 60
  seconds the two figures were 59,750 and 60,000; the move to 300 seconds makes
  them unmistakable.

## Deviations from the 2026-09-17 sequence, and one failed capture

- **Step 4 was not run**, and step 3 was run differently, as the baseline states.
  `LOGGER INTERVAL 60` would have reverted the 2026-10-02 cadence decision and
  cut storage headroom from about 16 days to about 3. `LOGGER INTERVAL 5` was
  sent instead and refused: `CMD_RESULT,LOGGER INTERVAL 5,ERROR`, exit 1,
  `Interval must be between 10 and 3600 seconds. Got 5`. The cadence read 300
  seconds at the start and at the end.
- **`post-lease-expiry` FAILED to capture what it existed for, and is kept.**
  `HOLD` succeeded and the command exited 0, but the capture ended on
  `port quiet for 600ms` before the 15-second lease could lapse. The default
  idle threshold sits below the firmware's 5-second heartbeat deliberately, which
  is right for a one-shot command and wrong for watching a later event. **Exit
  status 0 answered "did HOLD work", not "did we see the expiry"**, so a summary
  keyed on exit status would have called the step passed. Retried as
  `post-lease-expiry-2` with `--idle-seconds 8` and a 45-second cap. The failed
  attempt is retained because `capture-command.py` refuses to overwrite a label.
- **`post-step9-release` answered `NOT_HELD`, exit 1, and that is not a fault.**
  The 15-second lease had already lapsed in the gap between two separate host
  commands, so there was no session left to release. Step 9's own criteria both
  passed before it: `AUTONOMOUS OFF` kept the session
  (`A host session is held and is being KEPT`), and `AUTONOMOUS ON` while held
  was refused with `No change was made. The session is untouched.`

## Captures not wrapped by `capture-command.py`

`capture-command.py` refuses to pass `INTERVAL`, ` ON`, ` OFF`, `RESET` and
`CLEAR` to `send.sh`, by design: an acceptance capture must observe without
changing the board. Those four steps were sent directly and their output
redirected into this directory, which is **weaker evidence** — no argv, timing
or exit status is recorded alongside, and the SHA-256 below was computed after
the fact rather than by the wrapper:

```text
post-refusal-interval.txt    LOGGER INTERVAL 5        exit 1, refused
post-step9-hold.txt          LOGGER SESSION HOLD      exit 0
post-step9-off.txt           LOGGER AUTONOMOUS OFF    exit 0, session kept
post-step9-on-refused.txt    LOGGER AUTONOMOUS ON     exit 1, refused
post-step9-release.txt       LOGGER SESSION RELEASE   exit 1, NOT_HELD
post-step9-rearm.txt         LOGGER AUTONOMOUS ON     exit 0, re-armed
```

Checksums are in [MANIFEST.sha256](MANIFEST.sha256).

## State the board was left in

```text
[AUTO] Armed: YES          Interval: 300 seconds
[AUTO] Boot id: 44         RTC state valid: YES
[AUTO] Cycles this power-on: 7
[AUTO] Next sequence: 16519
[AUTO] Sequence reserved through: 16580
[AUTO] New records will carry experiment id: 3
```

## Limits

- The image is byte-identical to the pre-move build apart from four bytes of
  compile-time stamp, so this exercised the build, the upload and the
  single-definition property of the moved state. It exercised no new
  instruction.
- Damaged-log branches, a physical LittleFS fault, a failed open, a short read
  and induced accumulator overflow all remain **untested on hardware**.
- Board-reported CRC validity is not independent binary verification of the
  stored bytes.
- All epochs are 0 and all time quality `UNKNOWN`. The log cannot be dated from
  its own contents; `data/samples.csv` is the only wall-clock reference.
- Whether ESP-IDF v5.5.5 reloads `.rtc.data` on a non-deep-sleep reset remains
  open (D-060). D-061 removed the firmware's dependence on it and this stage did
  not pursue it.
- Storage headroom is unchanged in substance: room for about 4,664 more records,
  roughly 16 days, with no storage-full policy.
