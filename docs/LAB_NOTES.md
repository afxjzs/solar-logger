# Lab Notes

## Bench setup

The measured system is on a desk, not in a car. A jump-and-carry jump pack's battery, removed from its case, stands in for the BMW 12 V battery. The solar panel is in a window.

Every measurement in this file was taken against that arrangement. Irradiance is whatever the window gave at that time of day, so absolute solar numbers are not comparable across sessions and are not a model of the panel on a car. Current draw measurements of the XIAO and INA228 themselves are unaffected by this.

## 2026-09-18: Scheduler double-count stint - no hardware touched

**No hardware was touched.** Experiment 3 was not reset, storage was not
cleared, and nothing was uploaded or committed. Every result below is a host
test, a source read, or a compile. No firmware behavior is claimed as observed.

Fixed the scheduler double-count that the RELEASE/HOLD stint recorded earlier
the same day. The finding and its resolution are in [BACKLOG.md](BACKLOG.md),
and the rule is [DECISIONS.md](DECISIONS.md) D-046.

- The scheduler's `AUTO_SLEEP_HOST_RELEASE` path adds only the time since the
  handoff, which the handoff now records in `hostHandoffAtMs`. RELEASE and lease
  expiry share that path.
- The handoff measures with `millis()`. Its `micros()` form wrapped after 71.6
  minutes and dropped 4,294,967 ms from the session clock per wrap.
- The handoff decides whether it continues the retained clock before the RTC
  rebuild can set the magic.
- The timing arithmetic is three pure functions, compiled and run on the host by
  `tests/test_autonomous_schedule.py`. These are the project's first tests that
  execute firmware code.

### Firmware identity unchanged

```text
[FIRMWARE] Version:  0.3.0-dev
[FIRMWARE] Build ID: solar-logger-protocol-ack-v3
```

`0.3.0-dev` has never been committed or uploaded, so this change joins it. The
reasoning is in D-046.

### Validation performed

| Check | Result |
| --- | --- |
| `uv run pytest` | 153 passed, 1 xfailed (was 101 passed, 0 xfailed); the xfail is the overrun finding below |
| `uvx pyright --pythonpath .venv/bin/python` on `app/`, `tools/*.py`, `tests/`, `main.py` | 0 errors, 0 warnings, 0 informations |
| `arduino-cli compile --clean --warnings all --fqbn esp32:esp32:XIAO_ESP32C3 Arduino/solar-logger` | passed, no warnings; 1,102,133 bytes (84%, +118), globals 36,332 bytes (11%, +8) |
| host compile of the three timing functions | Apple clang 21.0.0, `-std=c++20 -Wall -Wextra -Werror`: passed |
| `tools/intellisense.sh` | the raw `.ino` compiles standalone with the editor's flags: ALL OK |
| `git diff HEAD --check` | clean; the untracked test and tool files were checked separately for trailing whitespace and have none |
| `tools/check.sh` | `ALL OK`, all six steps PASS |

### The regression tests were verified to fail

- **Against the saved pre-fix firmware**, the four source tests failed by
  assertion. The executed tests could not build their harness, because the
  helper functions do not exist there, so they errored rather than passed.
- **With the defect restored inside the helper**, the executed tests failed by
  assertion with the pre-fix arithmetic: 31,750 ms of sleep for a RELEASE 28 s
  after `setup()` entry, 31,996 ms for the same lease expiry, and an overrun for
  a 75 s session. A cold-boot release 45 s after `setup()` entry computed
  14,750 ms.
- **17 cases in a scratch copy**: the pre-fix firmware, 14 single-point
  mutations of the fix and the timing functions, and 2 behavior-preserving
  edits. All 15 regressions failed
  the intended test, 14 by assertion. The fifteenth, a new sleep path with no
  clock chosen, failed the host compile on `-Wswitch`, which is its intended
  failure. Reformatting every brace, and moving the helpers, the scheduler and
  the handoff into a `.cpp`, both left the suite green.

### Found, not fixed

Recorded in [BACKLOG.md](BACKLOG.md) under "FOUND 2026-09-18 IN THE SCHEDULER
STINT". Confirmed by reading, the first also by running the timing functions.

- **An overrun moves the interval boundary without resetting the
  accumulators.** At `LOGGER INTERVAL 10` every unclaimed wake overruns, and
  each record then claims about 10 s for about 20 s of charge. A strict `xfail`
  records it. It is not reachable at 60 s.
- **The host-session timing line still uses `micros()`** and misreports
  sessions over 71.6 minutes. Diagnostic text only.
- **After a handoff, the new boundary is taken after the interval close**, not
  at its accumulator reset. Small, unmeasured, and older than this change.

The secondary finding from the RELEASE/HOLD stint, arming while tethered, is
still open. This fix shares no code with it.

### Hardware acceptance still required - PENDING, nothing below has been run

This satisfies the precondition of the RELEASE/HOLD entry below, "after the
scheduler defect is fixed". After a deliberate `tools/upload.sh`, run that
entry's list, and add:

1. **The first sleep after RELEASE.** On a claimed timer wake, HOLD, send a few
   KEEPALIVEs, and RELEASE about 30 s after the claim. PREDICTED:
   `[AUTO] Sleeping until interval deadline; remaining:` just under 59,750 ms.
   That is 60,000 minus the 250 ms grace, minus the output between the handoff
   and arming the grace. The pre-fix image would print about 60,000 minus the
   whole time awake. This is the 2026-09-16 check 3 below, with the number
   predicted.
2. **The record after it.** PREDICTED: `interval_ms` near 60,000, like an
   unclaimed record. Compare the `session_elapsed_ms` difference between the
   last record before the session and the first after it against the host
   clock between those two wakes. PREDICTED: they differ only by oscillator
   drift over the sleeps and a bootloader time per wake. Neither has been
   measured, and D-019 forbids assuming a drift figure. Pre-fix, the record
   clock would also run ahead by the whole session length, tens of seconds or
   more, which is the difference this check can see.
3. **Lease expiry.** Hold a session and stop the keepalives. PREDICTED:
   `remaining:` a few milliseconds under 60,000, then the same record checks as
   in 2.
4. **A session past 71.6 minutes**, optional because it is slow. Hold with
   `app/solar_logger.py` for over 72 minutes, then press Control-C. PREDICTED:
   the check in 2 still agrees. Pre-fix, the record clock would be 4,294,967 ms
   short per wrap, and the double count would add the session length on top.
5. **Unclaimed wakes unchanged.** The 2026-09-17 step 8 below:
   `interval_ms` near 60,000, and `remaining:` near 50,000 after a 10-second
   rendezvous.

## 2026-09-18: RELEASE/HOLD correctness stint - no hardware touched

**No hardware was touched.** Experiment 3 was not reset, storage was not
cleared, and nothing was uploaded. Every result below is a host test, a source
read, or a compile. No firmware behavior is claimed as observed.

Fixed the four findings the characterization gate recorded earlier the same
day. The findings and resolutions are in [BACKLOG.md](BACKLOG.md); the rules
are [DECISIONS.md](DECISIONS.md) D-044 (firmware) and D-045 (host).

- **RELEASE** answers `OK` only when it completed: session ended, and if armed,
  handoff prepared and deferred sleep armed. A failed handoff answers `ERROR`
  and the console names the stage.
- **HOLD during the deferred-sleep grace** is refused with `ERROR`. The pending
  sleep has one arm and one cancel. `LOGGER AUTONOMOUS ON` now enters
  `DEEP_SLEEP_PENDING` for its grace, and an explicit stop cancels a pending
  sleep before its NVS write.
- **The logger** reports RELEASE from the machine RESULT only.
  `SessionClient` had been ignoring the machine RESULT for HOLD, KEEPALIVE and
  RELEASE, because the human line printed before it cleared the expectation.
- **A RESULT whose ACK was lost** is accepted and always flagged. A late ACK
  proves an earlier result stale. `tools/send.sh` now exits 0 for such an
  `OK`, with a distinct line and a warning, where it used to exit 1.

### Firmware identity changed

```text
[FIRMWARE] Version:  0.3.0-dev
[FIRMWARE] Build ID: solar-logger-protocol-ack-v3   (unchanged)
```

MINOR under the version policy, because RELEASE and HOLD changed what they do
or report. The build ID is unchanged, a judgment call recorded in D-044. Step 1
of the pending 2026-09-17 acceptance sequence below now expects `0.3.0-dev`.

### Validation performed

| Check | Result |
| --- | --- |
| `uv run pytest` | 101 passed, 0 xfailed (was 78 passed, 1 xfailed) |
| `uvx pyright --pythonpath .venv/bin/python` on `app/`, `tools/*.py`, `tests/`, `main.py` | 0 errors, 0 warnings, 0 informations |
| `arduino-cli compile --clean --warnings all --fqbn esp32:esp32:XIAO_ESP32C3 Arduino/solar-logger` | passed, no warnings; 1,102,015 bytes (84%, +1,042), globals 36,324 bytes (11%, unchanged) |
| `tools/intellisense.sh` | the raw `.ino` compiles standalone with the editor's flags: ALL OK |
| `tools/check.sh` | `ALL OK`, including `git diff HEAD --check` |

The former strict xfail, `test_release_can_report_a_failed_handoff`, turned
into an XPASS failure as soon as the fix went in, which is what strict mode is
for. The marker came off in the same change.

### The new tests were verified to fail

In a scratch copy, 21 reverts of individual pieces of the fixes each failed the
intended test. They covered the firmware result mapping, each handoff return,
the HOLD refusal and its status word, each pending-sleep writer, the stop
ordering, the arm-time interval clock, each host rule in `SessionClient`,
`SerialDevice` and `send.sh`, and the logger delegation. Reformatting every
brace and moving the five changed functions into a `.cpp` left the suite green.

### Found, not fixed

Recorded in [BACKLOG.md](BACKLOG.md) under "FOUND 2026-09-18 IN THE
RELEASE/HOLD CORRECTNESS STINT". Confirmed by reading only.

- **After a host handoff, the scheduler counts the awake time twice.** The
  handoff already puts the awake time into the retained session clock, and the
  scheduler adds it again. Predicted from the code: the first sleep after a
  RELEASE or lease expiry is short by about the time spent awake, while the
  next record's `interval_ms` still claims a full cadence. **This blocks the
  next upload**: the 2026-09-16 acceptance check expecting a remainder "near
  60000 ms" after RELEASE is predicted to fail.
- **Arming while tethered discards the open tethered interval** without closing
  it or saying so. Low impact.

### Hardware acceptance still required - PENDING, nothing below has been run

After the scheduler defect is fixed and `tools/upload.sh` is run deliberately:

1. The 2026-09-17 sequence below, with step 1 expecting `0.3.0-dev`. On an
   armed board its step 4 (`LOGGER INTERVAL 60`) answers `ERROR`, because the
   interval is refused while armed; that is correct, not a regression.
2. **RELEASE completes.** `tools/send.sh --wait LOGGER SESSION HOLD`, then
   `tools/send.sh LOGGER SESSION KEEPALIVE`, then
   `tools/send.sh LOGGER SESSION RELEASE`. Expect `CMD_RESULT,...,OK`, exit 0,
   `[AUTO] Deferred autonomous sleep ARMED`, the capture ending with the
   transport disappearing, and a record at the next timer wake.
3. **HOLD inside the grace is refused.** Hold a session, then put RELEASE and
   HOLD into one serial write, so the HOLD is parsed well inside 250 ms. No
   host tool can do this, by design; a direct pyserial write while nothing else
   owns the port does. Expect `CMD_RESULT,LOGGER SESSION RELEASE,OK`, then
   `CMD_RESULT,LOGGER SESSION HOLD,ERROR` with
   `HOLD not granted: autonomous sleep is pending`, then the board sleeping.
   Before this fix the source granted that HOLD with `OK` and then slept
   anyway; that was read, never observed.
4. **Stop inside the arm grace.** On an unarmed tethered board, one write of
   `LOGGER AUTONOMOUS ON` and `LOGGER AUTONOMOUS OFF`. Expect both `OK`,
   `Pending autonomous sleep CANCELED: LOGGER AUTONOMOUS OFF requested`, and
   the board staying awake and logging.
5. **The logger's release report.** With `app/solar_logger.py` holding an armed
   board, Control-C. Expect
   `[SESSION] Board released: ALL OK (firmware answered RELEASE with OK).`
6. **Lease expiry unchanged.** Hold, send nothing, and expect expiry near 15 s
   and a return to autonomous sleep.

RELEASE's `ERROR` path and the storage-unavailable path need an I2C or storage
fault to trigger, so the source tests remain their only cover.

## 2026-09-18: Pre-modularization characterization gate - no hardware touched

**No hardware was touched.** Experiment 3 was not reset, storage was not
cleared, nothing was uploaded, and **no firmware source changed**. Every result
below is a host test, a source read, or a compile. No firmware behavior is
claimed as observed.

The stint added a small characterization layer, so the monolith's current
behavior is the reference each extraction stage is checked against. The
decision and its rules are [DECISIONS.md](DECISIONS.md) D-043; the per-stage
procedure and hardware smoke test are in [BACKLOG.md](BACKLOG.md).

### What was added

- Ten test functions, 21 passing cases, across
  `tests/test_characterization_protocol.py`, `test_characterization_policy.py`
  and `test_characterization_record.py`.
- One strict `xfail` that records a new defect instead of freezing it.
- `test_source_totals_are_committed_only_after_a_successful_append` extended
  rather than duplicated. It now pins "advances exactly once", "the record and
  RTC get the same value", "provisional = previous + delta", and the complete
  set of writers.
- `tests/firmware_source.py`, which reads every sketch file as C++ tokens. The
  accounting module's existing source tests moved onto it, because they read
  only `solar-logger.ino` and matched exact tabs, and would have broken on the
  first file move.
- `tools/check.sh`, the combined local gate.

No policy helper was extracted. Everything that needed firmware logic was
characterizable from source, and extraction is the modularization itself.

### Validation performed

| Check | Result |
| --- | --- |
| `uv run pytest` | 78 passed, 1 xfailed (was 57 passed) |
| `uvx pyright --pythonpath .venv/bin/python` on `app/`, `tools/*.py`, `tests/`, `main.py` | 0 errors, 0 warnings, 0 informations |
| `python -m py_compile` on the same files | passed |
| `arduino-cli compile --clean --warnings all --fqbn esp32:esp32:XIAO_ESP32C3 Arduino/solar-logger` | passed, no warnings; 1,100,973 bytes (83%), globals 36,324 bytes (11%), identical to 2026-09-17 as expected with no firmware change |
| `bash -n` on all five `tools/*.sh` | passed |
| `git diff HEAD --check` | clean; the new untracked files were checked separately for trailing whitespace and have none |
| `tools/check.sh` | `ALL OK` |

### The tests were verified to fail

Each source test was run against deliberately broken copies of the repository,
one change at a time. 34 of 34 regressions failed the intended test, each by an
assertion and none by a reader error:

```text
the six 2026-09-17 defects, reintroduced (numbered as in the entry below):
  1 totals committed before the append     1 totals compound-assigned
  2 snapshot left uninitialized            3 AUTONOMOUS OFF result ignored
  4 POWER TEST STOP changes state first    4 broad suspension predicate restored
  5 NVS failure becomes exp 0              6 AUTONOMOUS ON refusal removed
and:
  unknown command answers OK               HOLD emits two results
  ACK moved after dispatch                 sleep-test variants swapped
  canonical command renamed                KEEPALIVE status word changed
  build ID changed                         identity not printed on timer wakes
  host treats any RESULT as success        send.sh exits 0 without completion
  host accepts a RESULT in place of ACK    energy committed twice
  record carries the old total             a fourth writer of the total
  idempotent success before the refusal    host session counted as ownership
  RELEASE sleeps before its result         lease expiry disarms instead
  handoff sleeps before closing interval   same-width record fields swapped
  signed field made unsigned               packing removed
  CRC covers the CRC field                 field written after the CRC
  validity no longer checks version        flag bit value changed
```

Three behavior-preserving edits left the whole suite green: every Allman brace
reformatted K&R with tabs expanded, five functions moved into a `.cpp` (including
`processCommand()` and `runAutonomousWakeCycle()`), and braces placed inside a
guard's comments and message.

### The gate's own failure path was verified

In a scratch copy with a failing test and an unused-variable warning injected,
`tools/check.sh` reported `FAIL` for pytest and for the firmware compile, with
the warning printed, `PASS` for the other four, `NOT ALL OK`, and exit 1. Two
consecutive runs gave the same result.

### Tooling finding: a cached build hides warnings

The first version of `tools/check.sh` compiled without `--clean`. Run a second
time on the same scratch copy with the same injected warning, the compile step
**passed and printed no warning**. That was observed. The cause is inferred and
not verified in arduino-cli's source: it reused the object from the first run
and never re-invoked the compiler for that file. `--clean` fixes it, and
`--help` documents it as "do not use any cached build".

So a `--warnings all` compile of unchanged source at a path that was built
before proves nothing about warnings. This stint's own first compile was that
kind, and the `--clean` compile above supersedes it. The clean compile shows
the current source has no warnings, so the earlier "no warnings" records for
this source stand.

### Found, not fixed

Recorded in [BACKLOG.md](BACKLOG.md) under "FOUND 2026-09-18". All confirmed by
reading; none observed on hardware.

- **RELEASE answers `OK` after a failed handoff.** The result is keyed on
  whether a session was held, not on whether the release completed. On the
  interval-close or storage failure path the board stays awake while the host is
  told `OK`. This is a D-041 violation, and the strict `xfail` records it.
- **A HOLD inside the 250 ms deferred-sleep grace is granted and then slept
  on.** None of the host tools can land a HOLD that fast.
- **The logger prints `Board released: ALL OK` for any RELEASE outcome**,
  including `NOT_HELD`.
- **Decision needed:** `SessionClient` accepts a `CMD_RESULT` whose `CMD_ACK`
  was lost (a deliberate 2026-09-17 fix) without logging that the ACK was
  missing.

One documentation correction: the modularization plan said
`validateInaForWake()` calls `autonomousDeepSleepAgain()`. It does not.

### Still pending on hardware

Everything in the entries below, unchanged. In particular the 2026-09-17
acceptance sequence has not been run, and the board still runs an image that
predates the revision field.

## 2026-09-17: Correctness stint - six MUST-FIX defects resolved, first tests added

**No hardware was touched.** Experiment 3 was not reset, storage was not
cleared, and nothing was uploaded. Everything below is a source, compile, or
host-test result. **No firmware behavior claimed here has been observed on
hardware**; the acceptance sequence that would observe it is at the end of this
entry and is pending.

All six defects recorded under "MUST FIX BEFORE MODULARIZATION" in
[BACKLOG.md](BACKLOG.md) were re-confirmed against current source and fixed. The
findings and their resolutions stay in BACKLOG; only results are recorded here.

### Firmware identity changed

```text
[FIRMWARE] Version:  0.2.0-dev
[FIRMWARE] Build ID: solar-logger-protocol-ack-v3
```

A minor bump, per the policy in [PROJECT.md](PROJECT.md): `CMD_RESULT` changed
what it reports, which is a change to what every command reports and to the host
protocol. The build ID exists precisely to mark a protocol generation, so it
moved too. `-dev` stays until an image is confirmed on hardware.

**The board is still running an image that predates all of this**, and still
predates the revision field, so it cannot identify itself. The first upload
after this stint makes that answerable permanently.

### Validation performed

| Check | Result |
| --- | --- |
| `arduino-cli compile --warnings all --fqbn esp32:esp32:XIAO_ESP32C3 Arduino/solar-logger` | passed, no warnings |
| firmware image size | 1,100,973 bytes, 83% of the app partition (was 1,097,445 before the stint) |
| globals | 36,324 bytes, 11% of DRAM, unchanged |
| `uv run pytest` | 57 passed |
| `uvx pyright --pythonpath .venv/bin/python` on all five app/tools Python files | 0 errors, 0 warnings |
| `python -m py_compile` on all Python files including tests | passed |
| `bash -n` on `send.sh`, `upload.sh`, `intellisense.sh`, `watch-port.sh` | passed |
| `git diff --check` | clean |

### The regression tests were verified to fail before the fix

A test that has never failed proves nothing. Each source-shape assertion was run
against a reconstruction of the pre-fix code and confirmed to fail:

```text
CAUGHT   defect 1: rtcAutoRunChargeUAh +=
CAUGHT   defect 1b: commit before append guard
CAUGHT   defect 2: uninitialized SensorReading
CAUGHT   defect 5: NVS failure -> exp 0
```

The running-total specification test was checked the same way: against the
pre-fix rule it produces a running total of 352 where 236 is correct, which is
exactly the first interval's 116 counted twice.

### Two host defects found while testing, not by the review

Both were found because a test asserted the documented behavior and the code
disagreed:

- **`SessionClient` discarded a `CMD_RESULT` whose `CMD_ACK` had been dropped.**
  The outcome was only processed if the ACK had been seen first. D-036 is
  explicit that a protocol line can be lost under HWCDC backpressure, so a lease
  the firmware had genuinely granted could go unrecorded, and the next KEEPALIVE
  would then be refused by a board the host thought it had never claimed. The
  outcome is now accepted for a command still awaiting its ACK.
- **A failed command was reported as possibly-truncated output.** Capture ended
  only on a successful result, so every `ERROR` waited out the full hard cap and
  then warned that output might be incomplete — pointing the reader at a
  reporting limit instead of at the failure. Termination now keys on "an outcome
  arrived", success still keys on `OK`.

`tools/send.sh` also now prints the firmware's response on the failure path. It
used to return before printing it, which left an operator with a failure and no
reason for it.

### Not done, deliberately

**No storage-full policy was implemented.** The stint brief asked whether one of
the six MUST-FIX defects covered it. It does not — that item is in BACKLOG /
LATER — and the policy it would implement is Section 9 of
[STORAGE_SYNC_DESIGN.md](STORAGE_SYNC_DESIGN.md), which is marked PROPOSED and
says of itself that it "should be confirmed rather than assumed". The decision
needed is written up in [BACKLOG.md](BACKLOG.md).

One consequence did improve as a side effect: a full partition no longer
corrupts the running totals as well as stopping the log, because the totals are
now committed only after a successful append.

**No modularization.** The RTC ownership correction from the review stint stands
as documentation only; no state moved.

### Hardware acceptance sequence - PENDING, nothing below has been run

This supersedes nothing in the entries below; those procedures remain pending
too. Run after a deliberate `tools/upload.sh`.

1. **Identity.** `tools/send.sh VERSION` — expect `0.2.0-dev`, a real revision
   hash, and `solar-logger-protocol-ack-v3`. A missing revision means the image
   did not come from `tools/upload.sh`.
2. **Experiment 3 storage survived the upload.** `tools/send.sh LOGGER STORAGE
   INFO` — expect the same record count and sequence range as before the upload,
   and an intact tail. A different result means the upload disturbed the
   filesystem, which is itself worth knowing.
3. **`CMD_RESULT` now means completion.** `tools/send.sh LOGGER INTERVAL 5` —
   out of range, so expect `CMD_RESULT,LOGGER INTERVAL 5,ERROR` and **exit 1**.
   Before this stint it returned `OK` and exit 0. This is the single most
   important check in the list, because everything else's reporting depends on
   it.
4. **A valid command still succeeds.** `tools/send.sh LOGGER INTERVAL 60` —
   expect `OK` and exit 0.
5. **Arming reports its own success.** On an unarmed board, `tools/send.sh
   LOGGER AUTONOMOUS ON` — expect `CMD_ACK`, then `CMD_RESULT,...,OK`, then
   **exit 0**, then the board sleeps. Before this stint the board slept without
   ever emitting a result and the host reported failure for a successful arm.
6. **Session lifecycle.** On a later timer wake: `tools/send.sh LOGGER SESSION
   HOLD` (waits for the rendezvous by default), then three
   `LOGGER SESSION KEEPALIVE`, then `LOGGER SESSION RELEASE`. Each must show
   machine ACK and `CMD_RESULT,...,OK` and exit 0. RELEASE must deliver its
   result before the port disappears, and report the teardown as expected.
   KEEPALIVE and RELEASE must not wait for a future rendezvous.
7. **Lease expiry.** Hold a session and stop sending keepalives. Expect expiry
   near 15 s with no multi-second Serial stall, and a return to autonomous
   operation.
8. **Return to timer-wake operation.** Collect at least five consecutive
   unclaimed records. Verify each `interval_ms` is near 60000, that
   `session_elapsed_ms` never decreases while `boot_id` is unchanged, and that
   the new `[AUTO] Commanded sleep for that interval:` line appears and reads
   near 50000 ms after a 10-second rendezvous.
9. **Defect 6.** Hold a session, send `LOGGER AUTONOMOUS OFF` (the session is
   kept, per D-031), then send `LOGGER AUTONOMOUS ON`. Expect a refusal with
   `CMD_RESULT,...,ERROR`, exit 1, the session still held, and **the board still
   awake**. Before this stint it deep-slept immediately and discarded the open
   tethered interval.
10. **Defect 4.** During a USB rendezvous, send `POWER TEST STOP`. Expect a
    refusal naming the autonomous state, `CMD_RESULT,...,ERROR`, and the next
    record's `interval_ms` and charge unaffected.
11. **`POWER TEST STOP` with nothing armed.** On an idle tethered board, expect
    it to say nothing was armed, to leave the accumulators alone, and for the
    open interval to close normally at its full length afterwards.

Defects 1, 2 and 5 are failure-path fixes and cannot be triggered on demand
without injecting an I2C or NVS fault. Their cover is the source assertions in
`tests/test_autonomous_accounting.py`; step 8 confirms the normal path still
produces correct running totals.

## 2026-09-17: Architecture review stint - no code changed

A read-only review of the firmware, the host tooling, and the storage and
accounting model. **No hardware was touched.** Experiment 3 was not reset,
storage was not cleared, nothing was uploaded, and no runtime source file was
modified. The only changes are to [BACKLOG.md](BACKLOG.md) and
[PROJECT.md](PROJECT.md).

Findings live in [BACKLOG.md](BACKLOG.md) under "Known bugs and issues", grouped
by when they should be fixed. They are all **confirmed by reading the source**;
none is a runtime observation, and the entries say so.

### Validation performed

| Check | Result |
| --- | --- |
| `arduino-cli compile --warnings all --fqbn esp32:esp32:XIAO_ESP32C3 Arduino/solar-logger` | passed, no warnings |
| firmware image size | 1,097,445 bytes, 83% of the app partition; globals 36,324 bytes, 11% of DRAM |
| `python -m py_compile` on all five Python files | passed |
| `pyright` against the project venv, all five Python files | 0 errors, 0 warnings, 0 informations |
| `bash -n` on `send.sh`, `upload.sh`, `intellisense.sh`, `watch-port.sh` | passed |
| JSON parse of the four VS Code / workspace files | passed |
| `git diff --check` | clean |

The compile was run to establish the size and warning state as facts rather than
because source changed; it did not. `pyright` is not installed in the repo or on
`PATH` and was run through `uvx pyright --pythonpath .venv/bin/python`. Without
that flag it cannot see the venv's packages and reports seven import errors,
which is a tooling artifact and not a regression.

### Counts re-measured, and one correction

`grep -c RTC_DATA_ATTR` on the sketch returns **12**, not the 11 recorded in the
modularization plan on 2026-09-16. Ten are `rtcAuto*`; the other two,
`rtcSleepTestMagic` and `rtcSleepTestCycle`, belong to the deep-sleep power test
(D-011). The plan assigned "all 11" to the `auto_state` module, which would have
put the power test's cycle counter under the autonomous module's magic guard.
The plan is corrected.

`rtcAutoCommandedSleepMs` is assigned once and read nowhere. That matters beyond
tidiness: the session clock is pre-advanced by the *commanded* sleep before
sleeping, so `interval_ms` and `session_elapsed_ms` are commanded values plus
measured awake time, not measurements of elapsed wall time. The firmware
therefore has no device-side way to answer the RTC-drift question in
[STORAGE_SYNC_DESIGN.md](STORAGE_SYNC_DESIGN.md) Section 12. Recorded in
[BACKLOG.md](BACKLOG.md).

### Nothing here supersedes the pending hardware work

Every acceptance procedure in the three 2026-09-16 entries below remains
pending, unchanged and unrun. In particular the board is still running whatever
image predates the revision field, so it cannot identify itself, and the
cadence, `session_elapsed_ms` monotonicity, RELEASE-teardown and lease-expiry
tests have still not been run on hardware.

## 2026-09-16: Tooling stint - IntelliSense, firmware identity, host types, send.sh

No hardware was touched. Experiment 3 was not reset, storage was not cleared,
and nothing was uploaded.

### IntelliSense: four separate defects, all confirmed by reading the build

The reported symptom was that ESP-IDF headers (`soc/soc_caps.h`,
`freertos/FreeRTOS.h`, `esp_sleep.h`, `esp_rom_crc.h`) failed while Arduino
headers (`Wire.h`, `Preferences.h`, `WiFi.h`, `LittleFS.h`) resolved, and that
reloading the window cleared it temporarily.

Confirmed against the generated database and the installed core, not inferred:

1. **The compile database describes a file nobody edits.** Arduino CLI's entry
   names `<build>/sketch/solar-logger.ino.cpp`. The previous tooling appended an
   alias whose `file` was the `.ino` but whose `arguments` still named the
   generated `.cpp` as the input, and it cloned entry `[0]` on the assumption
   that Arduino CLI always emits the sketch first.

2. **The reported split is exactly the flag split.** The sketch's compile
   command carries the Arduino library paths as plain `-I` flags and the entire
   ESP-IDF path as:

   ```text
   -iprefix <sdk>/include/  @<sdk>/flags/includes
   ```

   That response file was measured at 16,823 bytes holding 325
   `-iwithprefixbefore` entries. Every failing header lives behind it; every
   resolving header lives behind a plain `-I`. Nothing in the database exposed
   those paths any other way.

3. **cpptools excludes `**/.vscode` from its own file operations by default.**
   `C_Cpp.files.exclude` defaults to `{"**/.vscode": true, "**/.vs": true}` in
   cpptools 1.34.4's `package.json`. Both the database and a 254 KB generated
   near-duplicate of the sketch were inside `.vscode/arduino-build/`.

4. **The `.ino` was not a valid C++ translation unit.** Compiled directly with
   the fully expanded flag set plus `-include Arduino.h`, GCC reported 18
   distinct undeclared functions and no other errors. Arduino CLI synthesizes
   those prototypes into the generated `.cpp`; the real build therefore never
   saw the problem.

   This one had been masked. `C_Cpp.errorSquiggles` defaults to
   `enabledIfIncludesResolve`, which suppresses every other error while any
   include fails — so fixing only the includes would have uncovered a second
   wave of errors that had been present the whole time.

A fifth observation, worth recording: the generated `.ino.cpp` in the old build
directory was **stale**. It contained neither `FIRMWARE_BUILD_ID` nor
`COMMAND_PROTOCOL_WRITE_TIMEOUT_MS`, both of which are in the current source, and
nothing detected or reported that.

**Not established:** which of (2) and (3) the editor was actually tripping over
at any given moment, or why a window reload helped temporarily. Reproducing
cpptools' internal configuration selection was not possible from the command
line. The fix removes all four defects rather than betting on one.

### What replaced it

`tools/intellisense.py` expands every `@response` file and resolves
`-iprefix`/`-iwithprefixbefore` into absolute `-I` flags, so the editor entry
has no indirection left to follow. The build path moved from
`.vscode/arduino-build/` to `build/intellisense/`.

Then it **compiles the raw `.ino` with `-fsyntax-only` using exactly the flags
the editor is about to be handed, and refuses to write the database if that
fails.** That check earned its place immediately: the first version of this
work force-included a generated prototypes header, and the check caught that
those prototypes reference sketch-defined types (`AutoState`, `SensorReading`)
that cannot exist at the top of the file where a forced include lands. The
approach was wrong and the verification said so before anything shipped.

The eighteen forward declarations went into the sketch instead, which is where
they belong.

Observed:

```text
[INTELLISENSE] Expanded the compile command to 335 absolute include paths
[INTELLISENSE] No response file or -iprefix indirection remains in the editor entry
[INTELLISENSE] Verifying the editor configuration by compiling the sketch with it...
[INTELLISENSE] Editor configuration compiles the sketch cleanly: ALL OK
[INTELLISENSE] Entries: 80 (including the editor entry for solar-logger.ino)
```

`--check` was confirmed to report stale after the sketch changed and ALL OK
after regeneration.

### Firmware identity

Version, revision, and build ID are now three fields in
`Arduino/solar-logger/firmware_version.h`. See [DECISIONS.md](DECISIONS.md)
D-038.

Both revision branches were verified in the **linked image**, not just at the
source level:

```text
arduino-cli compile --build-property 'compiler.cpp.extra_flags=-DFIRMWARE_GIT_REV="deadbee-dirty"'
    -> strings in solar-logger.ino.elf: 0.1.0-dev, deadbee-dirty,
       solar-logger-protocol-ack-v2

arduino-cli compile            (no property)
    -> strings in solar-logger.ino.elf: 0.1.0-dev,
       "UNKNOWN - not injected by this build (use tools/upload.sh)"
```

The quoting was checked separately rather than assumed:
`riscv32-esp-elf-g++ -E '-DFIRMWARE_GIT_REV="abc1234-dirty"'` expands to
`const char *r = "abc1234-dirty";`, and arduino-cli execs the compiler directly
so the embedded quotes survive as one argument.

Flash use went from 1,097,445 to 1,097,405 bytes (83%) — the identity block and
the `VERSION` command cost nothing measurable.

### Host type errors

Pyright 1.1.414 against the project interpreter reported 3 errors in
`app/solar_logger.py` and 1 in `app/device_session.py`. All four were real:

- `preserved_limits` was `list | None` because `PLOT_STATE["auto_follow"]` was
  **read twice**, once to decide whether to capture axis limits and again a
  hundred lines later to decide whether to restore them. A Follow toggle
  between the two reads would capture and never restore, or restore what was
  never captured. Fixed by reading it once into a local.
- `now - serial_opened_at` on `float | None`. `ser` and `serial_opened_at` are
  set and cleared together, but nothing enforced it. Substituting `0.0` would
  have produced a silence age of several decades and closed the port; the
  invariant is now checked explicitly at the top of the connected state and
  reported as a logger bug.
- `SerialDevice._read_line()` dereferenced `self._serial` without the None
  check its caller has. Now raises `DeviceError` like every other read failure.

Annotating `update_graphs(axes: Sequence[Axes])` surfaced a fourth:
`mdates.date2num` is typed as returning `float | NDArray` because it also takes
sequences. It is given one datetime here, so the two call sites became one
`plot_x_coordinate()` helper that says so.

Final state: **0 errors, 0 warnings** across `app/solar_logger.py`,
`app/device_session.py`, `app/device_tool.py`, `tools/archive-data.py`, and
`tools/intellisense.py`.

No `# type: ignore` was added, and no None was coerced to a default.

### send.sh

Waiting for a rendezvous is now the default; `--no-wait` opts out; SESSION
KEEPALIVE and RELEASE never wait and refuse an explicit `--wait`. See
[DECISIONS.md](DECISIONS.md) D-040.

One regression was found and fixed while making the change: `tools/upload.sh`
calls `device_tool.py session hold` inside its retry loop and relied on it
failing fast. Under the new default it would have blocked indefinitely on a
port that vanished between discovery and the claim. It now passes `--no-wait`
explicitly.

Behavior confirmed with no board attached:

| Invocation | Result |
| --- | --- |
| `tools/send.sh --no-wait STATUS` | fails immediately, exit 1 |
| `tools/send.sh LOGGER SESSION RELEASE` | fails immediately with the session explanation, exit 1 |
| `tools/send.sh --wait LOGGER SESSION RELEASE` | refused, exit 2 |
| `device_tool.py send --timeout 2 -- LOGGER AUTONOMOUS STATUS` | waits, then times out, exit 1 |

### Validation performed

- `arduino-cli compile --warnings all` — passed, with and without the injected revision
- editor-flag `-fsyntax-only` compile of the raw `.ino` — passed
- `pyright` on all five Python files — 0 errors
- `python3 -m py_compile` on all Python files — passed
- `bash -n` on all four shell scripts — passed
- JSON validation of all four VS Code / workspace files — passed
- `git diff --check` — clean

**There is no automated test suite in this repository.** Nothing was run that
could be called a unit or session test, and none is claimed.

### Firmware behavior deliberately unchanged

`CMD_ACK`, `CMD_RESULT`, the bounded 100 ms protocol writer,
`Serial.setTxTimeoutMs(0)`, deferred `SESSION RELEASE` sleep, host lease
behavior, session accounting, autonomous cadence scheduling, and durable
storage semantics were not touched. The firmware changes are: eighteen forward
declarations, an include, the identity print, and one new `VERSION` command
branch that goes through the existing dispatch and result path.

### CORRECTION, same day: the modularization plan's cycle claim was wrong

The first version of the plan in [BACKLOG.md](BACKLOG.md) said there was
"exactly one hard cycle, `command` ↔ `autonomous`." That was based on spot
checks of a handful of call sites, not on a full graph, and it was wrong.

A complete call-graph pass over all 119 top-level functions, with `//` and
`/* */` comments **and string literals** stripped, found **three** mutual
pairs, every one of them through autonomous scheduling:

| Boundary | down | up |
| --- | ---: | ---: |
| commands ↔ autonomous scheduling | 6 | 2 |
| host sessions ↔ autonomous scheduling | 8 | 8 |
| measurement/accounting ↔ autonomous scheduling | 3 | 8 |

Stripping string literals mattered: without it,
`autonomousDeepSleepAgain()` and `runAutonomousWakeCycle()` appear to call
`setup()`. They do not — the text occurs inside `Serial.println(...)`
arguments. That artifact produced a fourth, non-existent cycle in an
intermediate run.

The real finding is better than the wrong one. **Autonomous scheduling is not a
layer**: its state half (`AutoState`, the RTC globals, record append) sits
below measurement and its orchestration half (wake cycle, rendezvous,
scheduler) sits above it. One box spanning levels 1 through 4 of an 8-level
stack is why every arrow through it read as bidirectional.

With that split plus `connection` separated from `host_session`, two cycles
remain and both run through the same single edge — `autonomousUsbRendezvous()`
(line 6177) and `autonomousColdBootMaintenanceWindow()` (line 6636) calling
`handleSerialCommands()`. Removing that one edge and re-running cycle detection
gives **0 cycles**. The plan and its extraction order have been rewritten
around the measured stack.

Nothing was refactored. This is a correction to the plan, not to the firmware.

### Still pending on hardware

Everything listed in the three entries below this one remains pending. Nothing
in this stint tested any of it, and the firmware on the board is still whatever
was last uploaded — which, since the revision field did not exist until now,
cannot be identified from the board itself. The first upload after this change
makes that answerable permanently.

## 2026-09-16: Experiment 3 cadence and session elapsed audit

The first five clean autonomous records from Experiment 3 were inspected
without resetting the experiment, clearing storage, or rewriting records:

```text
#0 seq=1412 boot=13 exp=3 elapsed_ms=748385 interval_ms=60005
#1 seq=1413 boot=13 exp=3 elapsed_ms=818401 interval_ms=70008
#2 seq=1414 boot=13 exp=3 elapsed_ms=888420 interval_ms=70009
#3 seq=1415 boot=13 exp=3 elapsed_ms=299700 interval_ms=60005
#4 seq=1416 boot=13 exp=3 elapsed_ms=369725 interval_ms=70011
```

All five records were CRC-valid, with `invalid=0` and `exp=3`.

The source audit confirmed that autonomous sleep used the complete configured
60-second interval after the wake measurement and 10-second USB rendezvous had
already elapsed. The rendezvous was therefore added to the next measured
interval, producing approximately 70 seconds. The firmware now targets the
interval deadline and sleeps only for its measured remainder.

The same source audit found the elapsed rollback: host release rebased
`rtcAutoSessionElapsedMs` to `millis()`. Because timer wake is a reboot,
`millis()` restarted near zero while `boot_id` remained the RTC-retained power
session identifier. The field is documented as monotonic within `boot_id`, so
the rebasing was a firmware bug, not an intentional session-relative reset.
Timer-wake release now continues the retained session clock.

Observed validation:

- `arduino-cli compile --warnings all --fqbn esp32:esp32:XIAO_ESP32C3 Arduino/solar-logger` passed after both changes.
- No hardware upload was performed.

The later host-session audit found that the shared deadline scheduler had one
path-selection defect: `AUTO_SLEEP_HOST_RELEASE` was using post-boot `millis()`
even when RELEASE or lease expiry occurred during a timer wake. The scheduler
could then compare that small value with the retained session deadline and
request an inflated sleep. The selector now treats timer-wake host handoff as
retained-session time too. No upload was performed for this correction.

The local build artifact inspected before this correction contained the normal
deadline diagnostic strings, so it was newer than the pre-cadence source. That
does not prove which image is currently on hardware. The runtime line
`[AUTO] Sleeping until interval deadline; remaining: ... ms.` is the definitive
test for the fixed scheduler; an old image will not print it.

The complete acceptance test is:

1. Set `LOGGER INTERVAL 60` and leave autonomous mode armed.
2. On an unclaimed wake, verify the record interval is near 60000 ms and the
   sleep diagnostic reports a remainder near 50000 ms after a 10-second
   rendezvous.
3. On a later wake, send `LOGGER SESSION HOLD`, renew it three times, then send
   `LOGGER SESSION RELEASE` while the port is actively owned. Verify RELEASE is
   acknowledged and the sleep diagnostic reports a remainder near 60000 ms
   from the newly established release boundary, not an inflated value.
4. Repeat with the host stopped or keepalives withheld. Verify lease expiry
   enters the same scheduler and the next autonomous record is near 60000 ms.
5. If the diagnostic is absent, the hardware is running an older image; if it
   is present but the handoff sleep is inflated, capture the printed retained
   elapsed, interval start, target, and sleep values for a new diagnosis.

Hardware acceptance remains pending. With `LOGGER INTERVAL 60`, leave the USB
rendezvous at 10 seconds, collect at least five consecutive unclaimed records,
and verify each `interval_ms` is approximately 60000 ms and that
`session_elapsed_ms` never decreases while `boot_id` is unchanged. Also test a
host `LOGGER SESSION HOLD` followed by `LOGGER SESSION RELEASE` across a timer
wake and verify the same invariant.

## 2026-09-16: RELEASE acknowledgement teardown race

Hardware reproduced a RELEASE command that disappeared without even delivering
`[COMMAND] Received: LOGGER SESSION RELEASE`, including when sent immediately
after a successful KEEPALIVE. Source confirms the race: `processCommand()`
printed the echo, `hostSessionRelease()` cleared the connection and performed
the accounting transition, then directly entered
`esp_deep_sleep_start()`. With nonblocking HWCDC output, the echo and success
lines could still be queued when USB was destroyed.

The fix preserves `Serial.setTxTimeoutMs(0)`. RELEASE now prepares the safe
handoff, prints `[SESSION] Released: ALL OK`, sets a pending transition, and
returns to the main loop. The loop waits at most 250 ms, then enters the same
autonomous scheduler. Lease expiry remains direct because it has no command
acknowledgement to protect.

Acceptance after a deliberate upload:

1. Send `LOGGER SESSION HOLD`.
2. Send `LOGGER SESSION KEEPALIVE` and wait for its successful response.
3. Immediately send `LOGGER SESSION RELEASE`.
4. Require `tools/send.sh` exit 0 and output containing both
   `CMD_ACK,LOGGER SESSION RELEASE` and
   `CMD_RESULT,LOGGER SESSION RELEASE,OK` before the port disappears.
5. Repeat the sequence five times and verify each board returns on a later
   timer wake.
6. Separately hold a session, stop sending keepalives, and verify lease expiry
   occurs at approximately 15 seconds without multi-second serial blocking.

## 2026-09-16: Command acknowledgement path audit

The two hardware observations are now explained by source and the installed
ESP32 Arduino core. `tools/send.sh` opens one direct reader before writing,
then reads from that same port; it does not start a late background reader.
Its `reset_input_buffer()` runs before the command write, so it can discard
older rendezvous or boot output but cannot discard bytes generated after the
write. The host was incorrectly treating the human diagnostic echo as the
only acknowledgement.

In ESP32 core 3.3.11, `HWCDC::write()` uses the configured timeout when adding
to its TX ring. With `Serial.setTxTimeoutMs(0)`, a full ring causes a short
write; Arduino `Print::print()` ignores that returned count. `HWCDC::flush()`
also has no wait budget when the timeout is zero. This explains both facts:
HOLD can execute and later print successfully while its first human echo is
missing, and RELEASE can queue output but lose USB before the host observes it.

The firmware now emits machine protocol lines independently of verbose text:

```text
CMD_ACK,LOGGER SESSION HOLD
CMD_RESULT,LOGGER SESSION HOLD,OK
```

Protocol lines retry short writes for at most 100 ms. `setTxTimeoutMs(0)` stays
unchanged for all other output. RELEASE emits its result before its existing
250 ms deferred-sleep grace; lease expiry has no command result to protect and
continues directly to sleep after accounting.

The host now requires both machine ACK and RESULT. A result observed before
USB disappears is retained as success and the teardown is reported explicitly
instead of being mislabeled as a parse failure. The human diagnostic echo is
still useful for people but is no longer protocol state.

The firmware build identity is `solar-logger-protocol-ack-v2`, printed at boot
and in `STATUS`.

Validation completed:

- warning-enabled Arduino CLI compile passed
- Python syntax compilation passed for the protocol and logger modules
- shell syntax checks passed
- no hardware upload was performed after this change

Acceptance after a deliberate upload:

1. Run `tools/send.sh --wait LOGGER SESSION HOLD` five times; each must show
   `CMD_ACK,...`, `CMD_RESULT,...,OK`, and exit 0.
2. Send KEEPALIVE, then RELEASE immediately. Each of five RELEASE cycles must
   show machine ACK and `CMD_RESULT,...,OK`, exit 0, report expected transport
   teardown, and return on a later timer wake.
3. Separately hold a session, stop sending keepalives, and verify lease expiry
   near 15 seconds without multi-second Serial blocking.

## 2026-09-09: Serial ownership and upload workflow

The upload workflow was bench-tested with both host states:

- With the Python logger running, `tools/upload.sh` detected the logger, requested release through `.upload-in-progress`, received `RELEASED`, uploaded successfully, and the logger reconnected.
- With the Python logger stopped, `tools/upload.sh` detected no logger, required no handshake, and uploaded successfully through the dynamically discovered modem port.

A detected logger that does not acknowledge release still aborts. The uploader does not fall through to esptool after a failed acknowledgment.

## 2026-09-09: Power measurement setup

Power source and wiring:

- 3xAA battery holder.
- XIAO ESP32-C3 powered through VUSB.
- USB physically disconnected during the actual power measurements.
- DMM inserted in series with the XIAO supply.
- INA228 fully connected unless a measurement says otherwise.

### INA228 comparison

Normal awake logger with XIAO and INA228 connected:

```text
approximately 28.4 mA
```

XIAO with INA228 completely electrically disconnected:

```text
approximately 18.6 mA
```

The approximate difference is:

```text
28.4 mA - 18.6 mA = 9.8 mA
```

**CORRECTION (2026-09-10): the 9.8 mA "INA228 subsystem consumption" inference is NOT VALID.** The two readings above are real and are kept here as observations. The subtraction between them is not.

The problem is that the two readings come from materially different *firmware* states, not just different hardware states. Removing the INA228 does not simply remove its supply current; it changes what the firmware does. With no INA228 on the bus, `verifyInaIdentity()` fails in `setup()` and the firmware halts in its FATAL loop, printing a line every five seconds. That state has no I2C traffic, no one-second sampling, no 60-second interval close, and no NVS checkpoint writes. The normal logger has all four. Subtracting one from the other therefore measures "logger running minus logger halted," with the INA228's own draw buried somewhere inside that difference.

Two independent facts now bound the real number, and both say 9.8 mA is far too large:

- The INA228 datasheet (SLYS021A) specifies IQ at **640 µA typical, 750 µA maximum** with VSENSE = 0 V. That is an order of magnitude below 9.8 mA.
- The whole system, with the INA228 attached and powered and the ESP32 in deep sleep, measures **1.07 mA total**. The INA228's share cannot exceed that, and the datasheet number sits comfortably inside it.

The 1.07 mA whole-system sleep measurement recorded later on this page constrains INA228-plus-sleeping-XIAO consumption far more directly than any subtraction between two differently-behaving firmware states.

An earlier test disconnected only the INA228 3V3 supply and changed the reading by about 0.04 mA. That result is also invalid for estimating INA228 power, because SDA, SCL, and GND remained attached and could back-power the INA228 through its signal pins.

### Automated Wi-Fi test

The firmware test was armed before USB was disconnected. It repeats:

```text
WIFI OFF for 30 seconds
WIFI ON for 30 seconds
WIFI OFF for 30 seconds
```

Wi-Fi ON means station mode enabled without association to an access point. The board ran from the external AA/DMM setup, so USB power was not part of the actual measurement.

Observed readings:

| Phase | Displayed current |
| --- | ---: |
| Boot | approximately 19.6 mA |
| First Wi-Fi OFF | 28.4-28.6 mA |
| Wi-Fi ON | approximately 30.5 mA |
| Second Wi-Fi OFF | 28.3-28.5 mA |
| Next Wi-Fi ON | approximately 30.5 mA |

Observed steady/displayed Wi-Fi STA-idle delta:

```text
approximately +1.9 to +2.1 mA with Wi-Fi enabled
```

Do not characterize this as the total Wi-Fi power cost yet. The DMM was on its 10 mA range. Short ESP32 Wi-Fi current bursts may not be visible or may be averaged by the meter. Transient behavior is not yet characterized.

### Reproduction procedure

1. Connect the INA228 and power the XIAO through VUSB from the 3xAA holder, with the DMM in series.
2. Keep USB connected only long enough to arm `POWER TEST WIFI`.
3. Confirm with `POWER TEST STATUS` that the test is armed.
4. Disconnect USB physically.
5. Apply external battery power and let the board reboot.
6. Observe the OFF, ON, OFF transitions and record the DMM reading.
7. Use `POWER TEST STOP` later when Serial access is restored to stop the test and clear its persisted flag.

The armed state is stored in NVS under a key separate from experiment state, so the test can start after USB disconnect and reboot without resetting or incrementing the experiment.

## Plotting Results

### PLOT-1: History reload resolved

`app/solar_logger.py` now validates `data/samples.csv`, selects the latest experiment with valid rows, and loads its recent 15-minute window at logger startup. Malformed historical rows produce warnings and are skipped.

The CSV remains the complete history. The 15-minute limit applies only to the in-memory Matplotlib display.

### PLOT-2: Reboot/upload x-axis overlap resolved

The live graph now uses the timezone-aware host `captured_at` timestamp for its x-axis. Firmware elapsed time can still restart after an ESP32 reboot or firmware upload, but that value no longer controls horizontal plot position.

The existing display-only NaN discontinuity marker remains in use for real Serial interruptions. It creates a visible break without adding a fake telemetry row.

The plot now also disables additive offsets and scientific notation on telemetry y-axes, so instrumentation values display directly. The x-axis uses local clock labels instead of raw ISO strings.

Future plotting work may still improve visual styling or history controls, but the two backlog items above are resolved by this implementation.

## 2026-09-09: Cumulative charge plot

The live Python UI now includes a fourth chart, `Accumulated charge (mAh)`. It uses `running_charge_mAh` from the firmware's completed 60-second `CSV_DATA` interval accounting and the interval row's host `captured_at` timestamp.

The logger reloads recent interval history from `data/intervals.csv` at startup using the same display-history window as sample plots. It does not create one-second cumulative-charge points or numerically integrate current on the host. This chart represents charge delivered through the INA228-measured solar path, not battery state of charge.

The application does not currently display or claim to measure voltage-derived battery SOC. Future robust SOC work remains open, likely using BMW IBS data and/or a rested-voltage model.

## 2026-09-10: Interactive plot preload and inspection

The logger now loads recent history from both `data/samples.csv` and `data/intervals.csv` before waiting for Serial data. It selects the latest experiment, validates both schemas, skips malformed rows with warnings, prints exact loaded counts and the history range, and warns when the newest history is stale.

The four-panel Matplotlib view keeps `captured_at` as its x-axis. The window now includes visible `Home`, `−`, `+`, and `Follow` buttons; the native toolbar remains available when the backend exposes it. Auto-follow is the default. `+` narrows the time window and `−` widens it without changing Follow. With Follow ON, the selected-width window shifts to the newest data; with Follow OFF, the current zoom remains stable. Toggle `Follow` or press `f` to change the state. Keyboard shortcuts `h`, `z`, and `p` mirror Home, zoom in, and zoom out. Hovering a line shows local time and the measured value. The display remains rolling-window data; CSV files are not truncated.

The live renderer now keeps persistent line artists and hover cursors, updating their data in place instead of clearing and rebuilding four axes on every sample. It also avoids a blocking pause after every redraw. This removes the main avoidable source of UI lag and lets the direct zoom controls operate on stable plotted objects.

Hover inspection uses the small `mplcursors` dependency managed by `uv`.


## 2026-09-10: Deep-sleep power test added

`POWER TEST SLEEP` was added to the firmware. It repeats a 30-second awake phase with Wi-Fi off and a 30-second true deep sleep, using `esp_sleep_enable_timer_wakeup()` and `esp_deep_sleep_start()`. The INA228 stays powered from XIAO 3V3 for this first test; VIN, GND, SDA, and SCL all remain connected.

This entry records the capability and the limitations found while building it. The measurement itself was taken the same day; results are in [2026-09-10: Deep-sleep power measurement results](#2026-09-10-deep-sleep-power-measurement-results) below.

### What the test is for

The comparison it was built for is the ~28.4 mA awake reading already recorded on 2026-09-09 against the deep-sleep reading with the INA228 still powered. That sleep current was the measurement goal, and it was taken the same day. Isolating the INA228's own contribution while the ESP32 sleeps remained a separate later experiment.

### Deep sleep wakes through reset semantics

`esp_deep_sleep_start()` does not return. The chip reboots and `setup()` runs from the top, so there is no code that runs during sleep and no path that returns from sleep into `loop()`. The state machine is split across the reset boundary: `setup()` decides whether an awake phase is starting and why, `loop()` decides when to sleep again.

The armed flag lives in NVS (`sleep_test_arm`) so it survives the USB disconnect and the switch to AA battery power. The cycle counter lives in RTC-retained memory, guarded by a magic value, so a 30-second sleep cycle does not burn flash endurance for a value that is meaningless after a power cycle. See [DECISIONS.md](DECISIONS.md) D-011.

### Interval accounting cannot survive this test

This was the substantive finding while building it, and it is a genuine limitation rather than a bug to fix later:

- `millis()` restarts at zero on every wake, so the 60-second interval timer never expires during a 30-second awake phase.
- `setup()` clears the INA228 accumulators on every boot, so the previous awake phase's hardware accumulation is discarded on each wake.
- The board is not awake during sleep, so there is no honest elapsed-interval time to report.

Interval accounting is therefore explicitly suspended for the duration of the test, announced when the awake phase starts, and reported by `STATUS` and `POWER TEST STATUS`. Experiment ID, interval number, running charge, and running energy are preserved unchanged. `CSV_SAMPLE` and `CSV_DATA` are suspended too, because `elapsed_seconds` would sawtooth backwards on every wake under an unchanged `experiment_id`. The five-second heartbeat keeps printing real INA228 values.

### Duty cycle is not exactly 30/30

Each wake costs roughly 3.5 seconds of extra awake time before the awake phase timer starts: `delay(1500)` for USB re-enumeration plus `delay(2000)` for the ADC to settle. Real awake time per cycle is therefore about 33.5 seconds against 30 seconds of sleep. The awake-phase banner prints the measured boot-to-awake-phase time so the actual duty cycle is observable rather than assumed. Do not compute an average current from an assumed 50% duty cycle.

### Reproduction procedure

1. Connect the INA228 normally and power the XIAO through VUSB from the 3xAA holder, with the DMM in series.
2. Keep USB connected only long enough to send `POWER TEST SLEEP`.
3. Confirm with `POWER TEST STATUS` that the sleep test is armed.
4. Disconnect USB physically.
5. Apply external battery power and let the board reboot.
6. Record the DMM reading during the awake phase and during the sleep phase.
7. Reconnect USB and send `POWER TEST STOP` during an awake phase. Serial commands cannot reach the board while it is asleep.

### Troubleshooting

If the meter shows a steady high reading with no alternation, the test is not running: check `POWER TEST STATUS` and the `[BOOT] Wake reason` line once Serial is back.

## 2026-09-10: Deep-sleep power measurement results

The deep-sleep power test was run on hardware. This is the measurement the previous entry was built for.

### Conditions

- XIAO ESP32-C3 and INA228 connected normally.
- XIAO powered externally from the 3xAA holder through VUSB, DMM in series.
- USB physically disconnected during the measurement.
- Wi-Fi OFF throughout.
- True ESP32 deep sleep with timer wake.
- Firmware alternating approximately 30 seconds awake and 30 seconds deep sleep.

### Measured

| Phase | Displayed current |
| --- | ---: |
| Initial boot | approximately 18.8-19.3 mA |
| Normal awake steady state | approximately 28.2-28.6 mA |
| Deep sleep steady state | 1.07 mA |

The sleep reading repeated across multiple cycles with essentially identical values. The awake reading agrees with the ~28.4 mA baseline recorded on 2026-09-09, and the boot reading sits in the same range as the ~19.6 mA boot figure recorded then.

Against the ~28.4 mA awake baseline:

```text
approximately 96.2% lower steady current in deep sleep
approximately 26x reduction
```

### Wake transient is NOT characterized

The DMM briefly displayed overload during some wake transitions. No peak-current value was captured, and none should be assumed or back-calculated from these numbers. Characterizing the wake transient needs an instrument faster than a handheld meter.

This is the same class of qualification already recorded for the Wi-Fi test, where the meter range may average short ESP32 current bursts.

### Verified: the INA228 remained powered during deep sleep

The 3V3 rail question was left open when the current measurement was first recorded, and it was then closed by direct measurement on the same day.

- The INA228 remained physically attached throughout the measurement.
- The XIAO 3V3 / INA228 VIN rail was probed directly, across multiple awake and deep-sleep cycles.
- The rail held steady at approximately 3.28-3.29 V for several minutes.

The INA228 was therefore powered continuously, including during the ESP32's deep-sleep phases. Whatever the ESP32 does in deep sleep, it does not drop the 3V3 rail the INA228 runs from.

**1.07 mA is therefore the measured steady supply current of the XIAO ESP32-C3 in true deep sleep with the INA228 breakout still powered.** The qualification that previously blocked that phrasing is resolved, and the figure can be quoted as such.

Note what this does and does not say about the INA228's own draw. The rail measurement confirms the part was powered; it does not decompose the 1.07 mA into ESP32 and INA228 shares. That decomposition is the next experiment.

### Host logger diagnostic noise during the test

The Python logger printed repeated `CONNECTED-BUT-SILENT` warnings during each awake phase, each followed by `Firmware data resumed: ALL OK`. These are false alarms caused by the test's intentionally reduced telemetry cadence, not by a measurement or serial fault. Recorded as a backlog item in [BACKLOG.md](BACKLOG.md).

## 2026-09-10: INA-OFF deep-sleep variant added

`POWER TEST SLEEP INA OFF` was added alongside the existing `POWER TEST SLEEP`. Both remain available; they are two variants of one test, and only one can be armed at a time.

This entry records how the variant works. It was measured the same day; results are in [2026-09-10: INA-shutdown deep-sleep measurement results](#2026-09-10-ina-shutdown-deep-sleep-measurement-results) below.

### What differs between the variants

Only what the INA228 does during the ESP32's sleep phase. Neither variant removes INA228 power, touches the XIAO 3V3 rail, or disconnects SDA/SCL.

| | `POWER TEST SLEEP` | `POWER TEST SLEEP INA OFF` |
| --- | --- | --- |
| ESP32 during sleep phase | true deep sleep | true deep sleep |
| INA228 during sleep phase | continuous conversion | its own shutdown mode |
| Hardware CHARGE/ENERGY accumulation | continues | stops |
| Solar measurement during sleep | preserved | sacrificed |
| Measured sleep current | 1.07 mA | 0.33 mA |

### How shutdown is entered

Through the MODE bits of `ADC_CONFIG`, per TI datasheet SLYS021A Table 7-6. The register is at address `1h`, reset `FB68h`, and its fields are MODE (bits 15-12), VBUSCT (11-9), VSHCT (8-6), VTCT (5-3), and AVG (2-0).

The datasheet documents **two** shutdown encodings, `0h` and `8h`. The firmware writes `0h` and its verification accepts either, because the question worth asking is whether the device reports a documented shutdown mode, not whether it echoes the exact pattern we chose.

Only the MODE field is replaced. VBUSCT, VSHCT, VTCT, and AVG are carried through unchanged, and the firmware explicitly checks that the non-MODE bits did not move during the write. If they had, the next wake would restore something other than the known-good configuration.

Decoding the project's own `ADC_CONFIG` value of `0xFB6B` against that table gives MODE `Fh` (continuous bus, shunt and temperature), VBUSCT/VSHCT/VTCT `5h` (1052 µs each), AVG `3h` (64 samples), which matches what the firmware's configuration block has always claimed it was.

### Datasheet prediction, not a measurement

INA228 specifications from SLYS021A:

```text
IQ    (active)     640 uA typical, 750 uA maximum, VSENSE = 0 V
IQSD  (shutdown)   2.8 uA typical, 5 uA maximum
TPOR  from shutdown mode   60 us device start-up time
```

Arithmetic on those numbers suggests shutdown should save roughly 640 µA, which against the 1.07 mA baseline would be a large fraction of the total. **That is a prediction from the datasheet, not a result.** Record what the DMM actually shows and compare afterwards.

The 60 µs start-up time is worth noting for a different reason: the existing 2-second ADC settle in `setup()` is four orders of magnitude longer, so the wake path needed no new delay.

### Measurement semantics while the INA228 is shut down

The datasheet states that registers can still be read and written in shutdown mode. That is precisely the hazard. No conversions are running, so voltage, current, power, and temperature registers hold values left over from before shutdown, and CHARGE/ENERGY accumulation has stopped.

The firmware refuses to present those as live readings. While shutdown is active, `STATUS` and the heartbeat both say the INA228 is shut down and report no values instead of printing stale ones.

### Reproduction procedure

Identical to the INA-continuous variant, except step 2 sends `POWER TEST SLEEP INA OFF`. Expect this sequence before each sleep:

```text
[POWER TEST] INA-OFF sleep test
[INA228] Entering shutdown mode...
[INA228] ADC_CONFIG before: 0xFB6B
[INA228] ADC_CONFIG after:  0x0B6B
[INA228] Shutdown mode verified: ALL OK
[POWER TEST] Entering ESP32 DEEP SLEEP for 30 seconds...
```

and on each wake:

```text
[BOOT] Wake reason: DEEP SLEEP TIMER
[POWER TEST] Woke from INA-OFF deep sleep: ALL OK
[INA228] Continuous measurement restored: ALL OK
```

The `ADC_CONFIG after` value above is what the arithmetic predicts from writing MODE `0h` into `0xFB6B` while preserving the other fields. Confirm it against what the board actually prints rather than assuming it.

If shutdown cannot be verified, the firmware prints an explicit error, does **not** enter deep sleep, and stops the test. A sleep current measured with the INA228 in an unknown state would be worthless.

## 2026-09-10: INA-shutdown deep-sleep measurement results

`POWER TEST SLEEP INA OFF` was run on hardware. Conditions were identical to the INA-continuous run recorded above: XIAO ESP32-C3 and INA228 connected normally, XIAO powered from the 3xAA holder through VUSB with the DMM in series, USB physically disconnected during the measurement, Wi-Fi off, true ESP32 deep sleep with timer wake.

### Measured

| Phase | Displayed current |
| --- | ---: |
| Initial cold-start awake | approximately 28.7-28.8 mA |
| Post-deep-sleep awake | approximately 28.3-28.4 mA |
| Normal awake steady state | approximately 28.2-28.6 mA |
| Deep sleep, INA228 continuous | 1.07 mA |
| Deep sleep, INA228 shutdown | 0.33 mA |

The saving from putting the INA228 into its own shutdown mode:

```text
1.07 mA - 0.33 mA = 0.74 mA
```

Against the awake baseline, deep sleep with the INA228 shut down is roughly a 98.8% reduction, about 86x.

### The datasheet prediction held

The predicted saving from SLYS021A was roughly 640 µA, from IQ 640 µA typical against IQSD 2.8 µA typical. The measured saving is 740 µA, which sits between the datasheet's typical and maximum IQ figures of 640 µA and 750 µA.

That agreement is worth stating plainly because it was a genuine prediction made before the measurement, not a number fitted afterwards.

### What the residual 0.33 mA is not

IQSD is specified at 2.8 µA typical and 5 µA maximum, so the INA228 can account for at most about 5 µA of the remaining 0.33 mA. The other ~0.325 mA belongs to the XIAO ESP32-C3 board itself: the ESP32-C3 in deep sleep plus whatever the onboard regulator and the rest of the board draw.

This is an inference from the measurement plus the datasheet, not a separate measurement. Isolating the board's own floor would need a different test.

### Key architectural conclusion

**Leaving the INA228 continuously measuring while the ESP32 sleeps costs roughly 0.74 mA, and buys continuous hardware CHARGE and ENERGY accumulation through the sleep window.**

**Shutting both down reaches roughly 0.33 mA, and measurement and accumulation are completely suspended for that period.**

That is the tradeoff, and it is now a measured one rather than an estimated one. Neither option is simply better: 0.74 mA is the price of not having a hole in the charge record.

### Cold-start awake current differs from post-sleep awake current

Cold start read approximately 28.7-28.8 mA, while awake phases following a deep-sleep wake read approximately 28.3-28.4 mA. The difference is roughly 0.4 mA and it was repeatable enough to notice.

No explanation is recorded here, because none has been established. Both boots run the same `setup()` path. Do not assume a cause; if this difference matters to a future power budget, it needs its own test.

### Wake transient still uncharacterized

The DMM again briefly displayed overload around some wake transitions. No peak-current value was captured. This is the same limitation recorded for the INA-continuous run and for the Wi-Fi test, and it will keep recurring until the transient is looked at with an instrument faster than a handheld meter.

## 2026-09-10: Autonomous-storage design audit

A design/audit pass for the autonomous local logging milestone. No code was changed and nothing was measured on hardware. The full design is in [STORAGE_SYNC_DESIGN.md](STORAGE_SYNC_DESIGN.md); recorded here are the two findings that change what should be measured next, plus the bench tests the design could not settle by reading.

### Finding: the current boot path would silently destroy sleep-period accumulation

The INA228 hardware CHARGE and ENERGY registers are read in exactly two places, both inside `closeMeasurementInterval()`. `setup()` never reads them and calls `resetInaAccumulators()` on every boot.

Deep sleep wakes by reboot, so an autonomous wake that reused the present `setup()` would clear the entire sleep interval's integral before anything read it. Every record would report near-zero charge and nothing would say why.

This is harmless today only because both deep-sleep power-test variants suspend interval accounting. Recorded as decision D-020: reads come before resets.

### Finding: awake time dominates the power budget, not cadence

Using the measured 28.4 mA awake and 1.07 mA deep-sleep figures, with the current 3.5-second boot path:

| Wake cadence | Average current |
| --- | ---: |
| 60 s | 2.66 mA |
| 300 s | 1.39 mA |
| 900 s | 1.18 mA |

Deep-sleep floor is 1.07 mA. At 60-second cadence the boot overhead alone costs more than the sleep it enables — roughly 2.5x the floor.

The 3.5 seconds is `delay(1500)` for USB enumeration plus `delay(2000)` for ADC settling. On a timer wake with USB disconnected and the INA228 already converting continuously, neither is needed. This is a stronger argument for restructuring the wake path than storage capacity was.

### Bench tests needed before implementation

1. LittleFS mount, open, append, close time per wake. If it costs hundreds of milliseconds it changes the storage recommendation, because awake time is the dominant power term.
2. Minimum achievable autonomous wake duration with the USB delay and ADC settle removed. The 0.3 s figure used in the design arithmetic is an assumption.
3. RTC drift on this board. Compare commanded sleep duration against host `captured_at` deltas over many cycles. Until this is measured, no absolute-time accuracy claim may be published.
4. Real LittleFS usable fraction, against the 90% planning assumption.
5. Whether CHARGE or ENERGY overflow at 5- or 15-minute intervals at realistic solar currents. `DIAG_ALRT` at register `0Bh` answers this directly: ENERGYOF bit 11, CHARGEOF bit 10, MATHOF bit 9.
6. RSTACC self-clearing, confirmed from a CONFIG readback rather than inferred.

### One thing confirmed from existing data rather than the datasheet

The datasheet says the CONFIG `RST` bit self-clears but says nothing about `RSTACC`. The committed telemetry settles it: across 1,174 intervals in `data/intervals.csv`, `interval_charge_mAh` stays around 1.5 mAh per interval instead of growing cumulatively, and `running_charge_mAh` advances by exactly the interval value each minute. If `RSTACC` were sticky, accumulation would either stop or never reset. It should still be printed rather than inferred.

### Timekeeping addendum: confirmed platform facts

Two things were verified from the installed core's `sdkconfig` rather than assumed:

```text
CONFIG_RTC_CLK_SRC_INT_RC=y            RTC slow clock is the internal RC oscillator
CONFIG_RTC_CLK_CAL_CYCLES=576          calibrated against the main crystal at boot
CONFIG_LIBC_TIME_SYSCALL_USE_RTC_HRT=y system clock backed by RTC + high-res timer
CONFIG_ESP32C3_TIME_SYSCALL_USE_RTC_SYSTIMER=y
```

The first says this board has **no 32.768 kHz crystal for the RTC** — it runs on an RC oscillator that IDF calibrates at boot. Good enough to schedule sleep, not precision wall-clock hardware, and it drifts with temperature and supply voltage. A logger in a car sees both.

The second says the build is configured so `gettimeofday()` is carried across deep sleep by the RTC counter. **That is configuration, not tested behavior**, and it is now bench test 4 in the design document. Until it passes, the sequence number stays the load-bearing mechanism and retained wall-clock is treated as expected-but-unproven.

Neither fact changes the storage design. Both change what may be claimed: no absolute-time accuracy figure belongs anywhere in this project until drift is actually measured, ideally at more than one ambient temperature.

## 2026-09-10: Autonomous logger bench prototype implemented

One sleep/read/store cycle is implemented as `LOGGER TEST AUTONOMOUS`. It compiles for `esp32:esp32:XIAO_ESP32C3` at 82% of the app partition with no warnings.

**Nothing has been run on hardware. No timing figures have been measured.** The prototype exists to produce them.

### Hardware test procedure

Run with USB connected first, to watch a few cycles, then optionally on battery.

1. `tools/upload.sh`
2. `tools/send.sh LOGGER STORAGE CLEAR YES` — the LittleFS partition has never been formatted, so this is required once. Mount deliberately does not auto-format.
3. `tools/send.sh LOGGER STORAGE INFO` — confirm the filesystem mounts and reports roughly 1.4 MB total.
4. `tools/send.sh LOGGER TEST STATUS` — confirm `Armed: NO`, interval 60 seconds.
5. `tools/send.sh LOGGER TEST AUTONOMOUS` — arms, resets accumulators, and sleeps immediately.
6. Watch several cycles. Each wake should print the `[AUTO]` block and the `[TIMING]` block.
7. `tools/send.sh LOGGER TEST STOP` during an awake window. The awake window is short by design, so this may need retrying; that is itself a finding worth recording.
8. `tools/send.sh LOGGER STORAGE DUMP` — decode the stored records.
9. `tools/send.sh LOGGER STORAGE INFO` — confirm record count and sequence range.

Stopping is expected to be awkward: the whole point is a wake short enough that there is little window to catch. If it proves impractical, note it — that is a real usability finding about the design, not a defect in the test.

### What to record from the run

- Every `[TIMING]` line, especially **LittleFS mount** and **record append+flush**. These decide whether LittleFS survives as the storage mechanism or whether a raw ring buffer is needed, because awake time dominates the power budget.
- **Total work (no Serial)** versus **Total timer-wake duration**. The first is the production-relevant number; the second includes Serial output that production would not do. (The second was renamed on 2026-09-11 when it was found to be measuring the wrong span.)
- The raw `DIAG_ALRT` value each cycle, and whether `ENERGYOF` or `CHARGEOF` ever set at 60 seconds.
- Whether `interval_ms` lands near 60000. Deviation is RTC oscillator drift and feeds the drift question directly.
- Whether the first wake after arming produces a sensible interval charge rather than near-zero. Near-zero would mean something still reset the accumulators before they were read.

### Power-loss recovery test, worth doing deliberately

With the test running, pull power mid-cycle, then restore it and run `LOGGER STORAGE INFO`. Expect either an intact tail or an explicit report of discarded trailing bytes. A silent result would be a defect.

### Sequence behavior to verify

Sequence numbers must never repeat across a power cycle. Gaps are expected and acceptable: blocks of 64 are reserved in NVS ahead of use, so a crash abandons the remainder of a block. `LOGGER STORAGE DUMP` shows the actual sequence progression.

## 2026-09-11: First autonomous run on hardware - storage worked, management did not

The autonomous logger produced durable records on real hardware for the first time. A later cold boot reported:

```text
[STORAGE] File bytes:     576
[STORAGE] Valid records:  8
[STORAGE] Sequence range: 1 -> 8
[STORAGE] Log tail is intact: ALL OK
```

Eight 72-byte records, 576 bytes exactly, sequences 1 through 8, tail intact. The read-before-reset ordering, the durable append, the CRC and tail scan, the experiment/storage isolation, and the refusal to auto-format all held up. That part of the design is confirmed, not assumed.

**No timing figures came out of this run.** The instrumentation was wrong (see below), so the numbers the design is waiting on still have not been measured.

### The board could not be stopped over USB

With the Python logger stopped, a retry loop on `tools/send.sh LOGGER TEST STOP` eventually printed:

```text
[SERIAL] Port: /dev/cu.usbmodem1101
[COMMAND] Sent: LOGGER TEST STOP
[COMMAND] ALL OK
```

The next boot then printed `[AUTO] Persisted autonomous bench test found after a cold boot.` The flag had not been cleared.

Root cause: `handleSerialCommands()` is called only from `loop()`, and an armed cold boot returned from `setup()` into deep sleep without ever reaching `loop()`. The command's bytes arrived and sat unread in the USB CDC receive buffer until deep sleep discarded them. No amount of retiming would have helped, because there was no code path that could parse the command.

The short awake window made this harder to diagnose but was not the cause. Even a perfectly timed command would have been ignored.

This is recorded as [DECISIONS.md](DECISIONS.md) D-021, and fixed with a 15-second cold-boot maintenance window.

### `send.sh` reported success for a command that never ran

`[COMMAND] ALL OK` meant only that the write to `/dev/cu.usbmodem1101` succeeded. It said nothing about whether the firmware read the bytes, and in this case the firmware never did.

The firmware already echoes `[COMMAND] Received: <COMMAND>` from `processCommand()`, so an acknowledgement was available and simply was not being checked. `tools/send.sh` now waits for that echo and reports `ALL OK` only when it appears, with an exit status to match. See D-022.

A second gap showed up immediately afterward: the script proved a command had been parsed and then exited before showing what the firmware said in reply, so `LOGGER TEST STATUS` and `LOGGER STORAGE INFO` were unreadable without starting the Python logger. Direct mode now keeps reading and printing after the acknowledgement, ending on a 600 ms quiet period or a hard cap of 2 seconds (10 seconds for `LOGGER STORAGE DUMP`). The stop reason is printed every time, and a capture that hits the cap warns that it may be truncated.

The 600 ms threshold was chosen against the firmware's measured output cadence rather than picked arbitrarily: `LIVE_SAMPLE_INTERVAL_MS` is 1000 ms and `HEARTBEAT_INTERVAL_MS` is 5000 ms, so 600 ms reads as quiet in both states. Inside the maintenance window `loop()` is not running, so there is no CSV at all and ordinary commands return in about a second, which is what keeps recovery commands from eating the 15-second window.

### Timing instrumentation was measuring the wrong span

A cold boot printed:

```text
[TIMING] Total wake duration: 0.014 ms (includes Serial output)
```

Fourteen microseconds for a multi-second boot. `autonomousDeepSleepAgain()` took its start time from an argument, and three of its four callers passed `micros()` evaluated at the call site, a few microseconds before the print. The function was faithfully reporting the interval between two adjacent lines of its own code.

The timer-wake path was less wrong, since it passed the top of `runAutonomousWakeCycle()`, but all four paths shared one label, so a cold-boot total and a timer-wake total were presented as the same measurement.

Every total is now measured from `setupEntryMicros`, captured on the first line of `setup()`, and each path prints a label that says what it is: `Total timer-wake duration`, `COLD-BOOT startup duration`, `Arm-to-sleep duration`, or `Degraded timer-wake duration`. The three non-wake labels carry an explicit line saying they are not wake timings and must not be compared against one. The `micros()` value at `setup()` entry is printed alongside, so the bootloader time that none of these figures include is visible rather than hidden.

Append timing is now broken into open, write, and flush+close, since a single combined number would not say which one dominates.

### Why the next sequence jumped to 131

Observed:

```text
Next sequence from log: 9
Next sequence from NVS reservation: 131
Using: 131
```

Reservation size is 64. The old code reserved unconditionally on every arm and every cold boot, and always took the next sequence as one past the reservation, so every restart consumed 65 numbers:

| Event | Next sequence | High-water after |
|---|---|---|
| `LOGGER TEST AUTONOMOUS` | 1 | 65 |
| 8 wake cycles, sequences 1-8 | 9 | 65 (no reservation needed) |
| First cold boot while armed | 66 | 130 |
| Second cold boot while armed | 131 | 195 |

The observed boot was the second cold boot after arming. Neither cold boot completed a wake cycle, which is why no records above 8 exist.

A wake cycle does not re-reserve until the block is exhausted, so the cycles themselves cost nothing. The whole jump came from restarts.

The gap-safety design is unchanged. What changed is that an intact log tail is now treated as the sequence authority, so a clean restart continues from record 9 instead of skipping past the reservation, and a reservation is written only when the current one does not already cover the next sequence. An empty, partial, corrupt, or out-of-order log still falls back to the NVS floor. See D-023.

### USB presence detection, investigated

On this board `Serial` is `HWCDC` (the USB Serial/JTAG peripheral; `ARDUINO_USB_MODE=1` and `ARDUINO_USB_CDC_ON_BOOT=1` for `XIAO_ESP32C3`). Two calls exist:

- `Serial.isPlugged()` calls `usb_serial_jtag_is_connected()`, a timer-based check for USB start-of-frame packets. The ESP32 core's own source comments state it has several milliseconds of tolerance and "is known to flap even on a healthy link".
- `Serial.isConnected()` additionally requires that the host has clocked bytes out of the TX FIFO. It reads false whenever no terminal holds the port open, even with USB physically attached.

Neither reliably answers "can an operator reach me right now", so neither gates anything. Both are printed as diagnostics when the maintenance window opens and closes and in `LOGGER TEST STATUS`. Several bench runs of that output are what would justify depending on either one later.

### Firmware state

Compiles for `esp32:esp32:XIAO_ESP32C3` at 1,079,785 bytes, 82% of the app partition, with no warnings under `--warnings all`. Not uploaded.

### Next hardware procedure

The existing eight records are deliberately preserved. Do not clear storage.

1. `tools/upload.sh`
2. `tools/send.sh LOGGER STORAGE INFO` - expect the same 576 bytes, 8 records, sequences 1-8, intact tail. A different result means the upload disturbed the filesystem, which is itself worth knowing.
3. `tools/send.sh LOGGER STORAGE DUMP` - decode and inspect the eight records from the first successful run.
4. `tools/send.sh LOGGER TEST STATUS` - confirm `Armed: NO` before arming anything.
5. `tools/send.sh LOGGER TEST AUTONOMOUS`
6. Watch several wake cycles and record every `[TIMING]` line.
7. Reset or power-cycle the board. The maintenance window should open and count down from 15 seconds.
8. `tools/send.sh LOGGER TEST STOP` during the window. It should now report `Firmware acknowledged command: ALL OK`, and the firmware should print `[AUTO] Autonomous test STOPPED.`
9. `tools/send.sh LOGGER STORAGE INFO` - confirm the new records appended after sequence 8 with no reuse.

What to check specifically:

- The first record of the new run should continue from sequence 9, not jump.
- `LittleFS mount` and `record append` sub-timings are the numbers the storage-mechanism decision waits on.
- `Total timer-wake duration` should now be a plausible multi-hundred-millisecond figure rather than microseconds, and the cold-boot total should be several seconds larger and labeled as such.
- The first interval after a cold boot should be close to one cadence, because the accumulators are now reset at the end of the maintenance window rather than inheriting startup charge.

## 2026-09-11: 65 autonomous records inspected - accumulation works, experiment context did not

`LOGGER STORAGE INFO` after the first hardware runs:

```text
Filesystem total: 1441792 bytes
Filesystem used:  16384 bytes
Filesystem free:  1425408 bytes
Record size:      72 bytes
Log bytes:        4680
Valid records:    65
Oldest sequence:  1
Newest sequence:  187
Trailing bytes:   0
Tail status:      INTACT
Room for 19797 more records
```

65 valid records, 72 bytes each, 4680 bytes, tail intact, no trailing bytes. The sequence range 1 to 187 against 65 records is the reservation behavior described in the 2026-09-11 entry above, from restarts that reserved a block without completing a cycle. Gaps, not losses.

### What the records confirm

The thing the whole architecture rests on works: **charge and energy accumulated in INA228 hardware survived ESP32 deep sleep and were read before anything reset them.**

```text
#0 seq=1 boot=1 exp=0 elapsed_ms=243577 interval_ms=60003
   V=12.892382 I_mA=1.099 P_mW=26.790 T_C=21.914
   dQ_uAh=116 dE_uWh=1579 Qsum_uAh=116 Esum_uWh=1579
   time=UNKNOWN epoch=0 flags=0x4

#1 seq=2 boot=1 exp=0 elapsed_ms=303591 interval_ms=60006
   V=12.893359 I_mA=11.312 P_mW=145.843 T_C=21.921
   dQ_uAh=116 dE_uWh=1584 Qsum_uAh=232 Esum_uWh=3163
```

Observed, not predicted:

- Interval durations of 60003 to 60006 ms against a 60000 ms commanded cadence. Roughly 0.01% over one minute, which is the awake time being added to each interval rather than oscillator drift; separating the two still needs the drift test in Section 12.
- First-minute interval charge 116 µAh and energy 1579 µWh.
- Running totals summing correctly across records: 116, 232, 352, 472 µAh.
- Bus voltage stable around 12.892 to 12.894 V, die temperature around 21.9 °C.

Record #0 shows `I_mA=1.099` against `#1 I_mA=11.312` while `dQ_uAh` is 116 in both. The snapshot is one instantaneous reading taken at the wake, and the interval charge is the hardware integral over the whole minute. They are different quantities and are not expected to track each other.

### Bug: records stored exp=0 while the board was running experiment 2

Confirmed from source, not inferred. `experimentId` is declared at [solar-logger.ino:371](../Arduino/solar-logger/solar-logger.ino#L371) initialized to 0, and the only thing that populates it is `loadCheckpoint()`, called from `setup()` well below the autonomous timer-wake branch. A timer wake reaches `runAutonomousWakeCycle()` before that line ever runs, so the record was stamped with the startup value.

Nothing errored. The field was populated, well-formed, and CRC-correct. It was simply wrong, which is the failure mode that survives every check a reader might apply.

This is a **development bug in the prototype firmware, not filesystem corruption**. The stored bytes are exactly what the firmware wrote, and their CRCs verify.

The fix adds `loadExperimentIdOnly()`, a read-only NVS helper that reads one key with almost no Serial output, called after `Wire.begin()` and before any INA228 access. Read-only NVS cannot disturb the CHARGE and ENERGY registers, so the protected ordering is unchanged. See [DECISIONS.md](DECISIONS.md) D-024.

### Historical boundary in the log

The existing 65 records are deliberately left alone. Their CRCs are untouched and they remain valid evidence from the prototype.

```text
seq 1 .. 187, exp=0    prototype records, written before the fix.
                       exp=0 is a known bug, NOT experiment 0.
seq 188 onward, exp=2  post-fix records, real experiment attribution.
```

Any host reading this log has to treat the boundary as real. There is no way to tell a buggy `exp=0` from a legitimate one within a record, which is exactly why the fix marks unknown attribution explicitly instead of writing a plausible number.

### flags decode

`flags=0x4` on record #0 is `FIRST_AFTER_BOOT` with `TIME_QUALITY = UNKNOWN`. `flags=0x0` on the rest is the same time quality with no other bit set; `UNKNOWN` is zero, so it adds nothing to the byte. Neither indicates a problem.

Bit definitions are now written down in [STORAGE_SYNC_DESIGN.md](STORAGE_SYNC_DESIGN.md) Section 5, and `LOGGER STORAGE DUMP` prints decoded flag names next to the hex so a dump no longer needs the table.

### Field audit

Every other `AutoRecord` field was checked against the timer-wake path rather than assumed correct. `experiment_id` was the only one affected. The scaling constants used by `readSensor()` and the accumulator readers (`CURRENT_LSB_A`, `CHARGE_LSB_C`, `ENERGY_LSB_J`, `VBUS_LSB_V`, `DIETEMP_LSB_C`) are all `constexpr`, so they need no runtime initialization.

## 2026-09-11: USB rendezvous implemented - BLOCKING GATE NOT YET PROVEN

### What the hardware already proved, and what it did not

Durable records grew from 65 to 99 while the board was unreachable over USB. That proves the timer wake itself works:

```text
deep sleep -> timer wake -> read INA accumulation -> append durable record
           -> deep sleep -> repeat
```

**It does not prove that USB becomes usable on a timer wake.** Those are separate claims and the records only support the first one. The board writing records while nobody could reach it is, if anything, evidence for the opposite.

### The blocking question

```text
ESP deep sleep -> TIMER wake -> USB CDC becomes usable by macOS -> host connects
with NO reset, NO BOOT button, NO power cycle, NO USB replug
```

**This has never worked on hardware and is not proven by anything currently recorded.** Nothing below should be treated as working until it is.

What can be said from source, and only from source:

- Nothing in the firmware suppresses USB on a wake. There is exactly one `Serial.begin(115200)`, on the third line of `setup()` and well before the autonomous branch. No `Serial.end()`, no GPIO isolation, no sleep GPIO configuration anywhere.
- Deep sleep is a full reboot, so `setup()` re-initializes the USB Serial/JTAG peripheral on every wake the same way it does on a cold boot.
- The rendezvous gives the host 10 seconds, against roughly 1 to 2 seconds for macOS enumeration on this board when cold-booted.

All three are reasons to expect it to work. **None of them is a measurement**, and whether the ESP32-C3's USB Serial/JTAG peripheral re-enumerates to a macOS host after a deep-sleep wake is exactly the thing that has to be observed rather than reasoned about.

### Rendezvous window

`AUTONOMOUS_USB_RENDEZVOUS_MS = 10000`. Ten seconds, chosen to be unmissable rather than efficient. A window tuned for power would confuse "USB never came up" with "USB came up too briefly to catch", and those need different fixes.

It opens only after the measurement is stored and the accumulators are reset, so nothing about it can put an unstored interval at risk. Once per second it prints:

```text
[AUTO] Rendezvous: N / 10 s
[AUTO] USB plugged: YES/NO
[AUTO] CDC connected: YES/NO
[CONNECTION] Active transports: NONE
[CONNECTION] Host claimed: NO
```

A 10-second rendezvous on a 60-second cadence is a sixth of the duty cycle awake and will cost real power. That is accepted while the behavior is being proved. Measurement work and rendezvous cost are timed and printed separately so the price is visible from the first run.

### FIRST ACCEPTANCE TEST - USB enumeration alone

Do not involve `app/solar_logger.py`. Test one thing.

On the Mac:

```bash
while true; do
    date '+%H:%M:%S'
    ls /dev/cu.usbmodem* 2>/dev/null || echo "NO USB PORT"
    sleep 0.2
done
```

Then:

1. `tools/upload.sh`
2. `tools/send.sh LOGGER TEST STATUS` - confirm `Armed: NO` and experiment id 2
3. `tools/send.sh LOGGER TEST AUTONOMOUS`
4. Board sleeps. **Do not touch it.**
5. Wait for the timer wake.

PASS requires all of:

- `/dev/cu.usbmodem*` appears on its own
- it stays for about 10 seconds
- it disappears again only after the unclaimed rendezvous expires
- the cycle repeats for at least **two** timer wakes
- **no reset, no BOOT button, no power cycle, no replug**

**If the port never appears, stop there.** The remaining work is host-side lease logic built around a rendezvous that does not exist, and building more of it would be building on an unproven foundation. Diagnose ESP32-C3 USB behavior after a deep-sleep timer wake instead.

### SECOND ACCEPTANCE TEST - claiming a wake

Only after the first test passes. Start this before a wake is due and do not touch the board:

```bash
while true; do tools/send.sh LOGGER SESSION HOLD && break; sleep 0.2; done
```

PASS if the wake creates the port and `send.sh` reports the exact firmware acknowledgement. Then check `tools/send.sh LOGGER SESSION STATUS` shows:

```text
[CONNECTION] Active transports: USB
[CONNECTION] Any host connected: YES
[SESSION] Host session held: YES
```

and that the board is still awake well past the 10-second rendezvous. Then `tools/send.sh LOGGER SESSION RELEASE`, and confirm transports return to `NONE`, autonomous mode is still **ARMED**, the board sleeps, and the next timer wake exposes USB again.

Note that a manual `HOLD` from `send.sh` is not renewed by anything, so the 15-second lease expires on its own. That is deliberate: a command typed once must not be able to hold the board awake forever. Expiry is itself worth watching, because it exercises the crash-safety path.

### Python logger integration is phase 3

`app/solar_logger.py` is deliberately **unchanged**. Automatic claim, keepalive, and release come after the two tests above pass. Wiring a host-side lease into the logger before the rendezvous is proven would only make a failure harder to localize.

## 2026-09-11: USB rendezvous PASSED on hardware, and two bugs it exposed

### CONFIRMED MILESTONE - the blocking gate is passed

With **no reset, no BOOT button, no power cycle and no USB replug**:

```text
deep sleep -> TIMER wake -> USB rendezvous
           -> tools/send.sh LOGGER SESSION HOLD -> firmware ACK
           -> HOST_SESSION
           -> tools/send.sh LOGGER SESSION RELEASE -> deep sleep
```

Timer-wake USB re-enumeration and explicit host claiming both work on real hardware. This was the gate, and everything above it in the design now rests on something measured rather than assumed. **Do not regress this.**

### BUG 1: the 15-second lease never expired

Observed: `HOLD` advertised a 15-second lease, zero keepalives were sent, and the session was still alive 74 seconds later. Only `RELEASE` put the board to sleep. The crash-safety design was therefore not working at all.

**Root cause, confirmed from the ESP32 core source rather than inferred.** `HWCDC` defaults to `tx_timeout_ms = 100` with `max_consec_timeouts = 20`, so one blocked `Serial` write stalls for roughly **two seconds** once the 256-byte TX ring buffer fills and nothing drains it. That is exactly the state after `send.sh` captures a few seconds of response and closes the port: `isPlugged()` keeps reporting true, so the driver keeps waiting rather than giving up.

`setup()` prints well over a hundred lines after the claim. At up to two seconds each that is the missing ~74 seconds, spent inside `setup()`. The only lease check lived in `loop()`, which was never reached. When `setup()` finally finished, the `RELEASE` bytes were already sitting in the RX buffer, and `handleSerialCommands()` runs first in `loop()` — so the lease check never got a single turn.

Fixes:

- `Serial.setTxBufferSize(4096)` before `begin()`, sixteen times the default, so overflow is rare rather than routine.
- `Serial.setTxTimeoutMs(0)`, so a departed host can never stall the firmware. Output is dropped instead of waited on. Diagnostics must never be allowed to stall measurement, sleep decisions, or lease expiry, and text nobody is draining has no recipient anyway.
- The 1500 ms USB delay and 2000 ms ADC settle are skipped when a host session is held. USB is demonstrably up, the INA never stopped converting, and 3.5 seconds of a 15-second lease is not affordable.
- Lease expiry moved into `serviceHostLease()` so the check is not welded to one call site.

### BUG 2: release closed a 9 ms interval after a 74-second session

Observed: `[AUTO] Closing the open tethered interval first (0.008 s)` and `Interval charge: 0.000047 mAh`.

**Root cause.** The tethered baseline was established at the very end of `setup()`, hundreds of lines after the claim, and `setup()` called `resetInaAccumulators()` on the way past. Everything accumulated between the autonomous reset and that second reset was discarded without a word. With Bug 1 stretching `setup()` to 74 seconds, that discarded the entire session, and the 9 ms interval was just the sliver between the late reset and the already-queued `RELEASE`.

**About 74 seconds of charge was silently lost.** The instinct that measurement time had disappeared was correct.

Fix, matching the required semantics exactly:

```text
HOLD:     discard and DOCUMENT rendezvous accumulation
          -> reset INA accumulators
          -> START tethered interval timestamp
          -> HOST_SESSION
SESSION:  normal accounting runs
RELEASE / EXPIRY:
          -> close the tethered interval at its REAL elapsed duration
          -> save experiment accounting
          -> establish clean autonomous baseline
          -> deep sleep
```

`setup()` now skips its reset entirely when `HOLD` already established the baseline, and says so rather than doing it silently.

**A short final interval is not always a bug.** When a normal 60-second interval closes moments before release, the remainder legitimately is milliseconds, and the session's charge is accounted across all the intervals that closed during it. Release now prints the session length and how many ordinary intervals closed inside it, so that case can never again be mistaken for lost measurement.

### TEST A: lease expiry, no keepalives

1. `tools/upload.sh`
2. `tools/send.sh LOGGER TEST AUTONOMOUS`
3. Wait for a timer wake, then catch it: `while true; do tools/send.sh LOGGER SESSION HOLD && break; sleep 0.2; done`
4. **Send nothing else. Touch nothing.**

PASS requires:

- at roughly 15 seconds the firmware expires the lease by itself
- it prints `[SESSION] Host lease EXPIRED after N ms without keepalive`, `[CONNECTION] USB transport RELEASED due to lease expiry`, and the `HOST_SESSION -> HOST_LEASE_EXPIRED` transition
- the USB port disappears because the board sleeps
- the next timer wake exposes USB again automatically

No `RELEASE` is sent during this test.

### TEST B: keepalives and honest session accounting

1. Catch the next wake with `HOLD`
2. `tools/send.sh LOGGER SESSION KEEPALIVE` every ~5 seconds for at least 30 seconds
3. `tools/send.sh LOGGER SESSION STATUS` partway through
4. `tools/send.sh LOGGER SESSION RELEASE`

PASS requires:

- the board stays awake across several 15-second lease periods
- `STATUS` reports a sane lease remaining, time since the last HOLD/KEEPALIVE, and the tethered interval open time
- release closes an interval **approximately equal to the time since the baseline was established**, not milliseconds, with plausible charge for that duration
- if a 60-second interval closed just before release, the printed session length and interval count account for the difference
- autonomous mode stays **ARMED**, the board sleeps, and the next timer wake happens on its own

The host-session lifecycle is not complete until both tests pass.

## 2026-09-11: Test B PASSED, autonomous promoted to a real mode

### TEST B CONFIRMED on hardware

```text
HOLD -> 3 KEEPALIVEs -> STATUS -> RELEASE
```

`STATUS` mid-session reported a sane lease remaining, time since the last renewal, and a tethered interval age that tracked the session. `RELEASE` closed a **25.517-second** tethered interval with 0.089697778 mAh and 1.165482667 mWh, average 12.655 mA and 164.429 mW. Autonomous mode stayed **armed** and the board returned to deep sleep on its own.

The accounting fix works: a 25-second session closed a 25-second interval, not a 9-millisecond one.

### TEST A STILL UNPROVEN

```text
HOLD -> zero KEEPALIVEs -> zero RELEASE -> ~15 s -> firmware expires its own lease
```

Until that runs, **crash safety is unproven**. It is the only path that covers a host that dies, a cable pulled, or a laptop suspending, and those are the cases a lease exists for. A passing Test B says nothing about it: Test B exercises the cooperative path and Test A exercises the one where the host does nothing at all.

### The interval-count diagnostic bug

`[AUTO] Session lasted 25.521 s; normal 60 s intervals already closed during it: 2350`

2350 was the restored interval number, not a count. `hostSessionHold()` runs during the USB rendezvous, which is before `loadCheckpoint()` restores `completedInterval` from NVS, so the captured baseline was always 0 and the subtraction returned whatever NVS held.

Same class as D-024, and introduced while fixing D-024. Replaced with `hostSessionIntervalCloses`, incremented inside `closeMeasurementInterval()` at the actual event, which cannot be wrong about initialization order. A 25-second session now reports 0.

### Commands promoted

`LOGGER AUTONOMOUS ON` / `OFF` / `STATUS` are canonical. The `LOGGER TEST *` spelling still works and prints a deprecation notice. Procedures recorded in earlier entries above keep the names that were actually typed at the time, so the record stays accurate; use the canonical names for anything new.
