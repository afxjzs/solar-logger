# Lab Notes

## Bench setup

The measured system is on a desk, not in a car. A jump-and-carry jump pack's battery, removed from its case, stands in for the BMW 12 V battery. The solar panel is in a window.

Every measurement in this file was taken against that arrangement. Irradiance is whatever the window gave at that time of day, so absolute solar numbers are not comparable across sessions and are not a model of the panel on a car. Current draw measurements of the XIAO and INA228 themselves are unaffected by this.

## 2026-10-02: Retained-state extraction ACCEPTED on hardware (D-063)

Full acceptance of `1801098` on image `8faa0e3`, run by the orchestrator the same
day the module was built. **PASSED on every criterion.** Evidence, criteria
written before any result, and limits:
[logs/evidence/2026-10-02/retained-state-1801098/](../logs/evidence/2026-10-02/retained-state-1801098/acceptance-result.md).

Experiment 3 was not reset, cleared, erased or corrupted at any point.

| Fact | Value |
| --- | --- |
| Records, post | 15,106, sequences 1412–16517 |
| Sequence gaps | **none, across the whole history** |
| Board invalid | 0 |
| Preflight lines reproduced byte-identically | **15,091 of 15,091** |
| `validate-dump.py --compare` | exit 0 |
| `check-totals-continuity.py --compare` | exit 0, boundary **identified** from the preflight dump |
| Forbidden strings | all ten absent from all 20 captures |

**The sharpest result: `FIRST_AFTER_BOOT` on exactly one record out of six
consecutive unclaimed wakes.** That flag is set when `rtcAutoCycleCount` reads 0.
A retained definition duplicated across translation units — the hazard BACKLOG
calls the single highest-risk detail in the modularization plan — would have set
it on every wake. One in six is positive proof of a single definition surviving
five deep sleeps. Nine of the ten relocated variables are directly evidenced;
`rtcAutoCommandedSleepMs` cannot be, for the structural reason D-063 records.

**Two results beyond the stage under test:**

- **The project's first hardware lease-expiry trace**, outstanding since
  2026-09-11 in both PROJECT and this file: `Host lease EXPIRED after 15000 ms
  without keepalive`, the firmware releasing the USB transport itself, the open
  tethered interval closing (D-026), and a return to autonomous sleep.
- **D-046's handoff asymmetry, side by side for the first time.** `RELEASE` slept
  299,750 ms, one cadence minus the 250 ms result grace; lease expiry slept
  300,000 ms, a full cadence, having no result to deliver. At 60 seconds these
  were 59,750 and 60,000; the move to 300 made them unmistakable.

**Three findings the acceptance itself produced, all recorded in BACKLOG:**

- `tools/storage-smoke.py` hardcoded a 150-second wait timeout written for the
  60-second cadence. At 300 it would fail a stage roughly half the time, and a
  timed-out capture looks exactly like a board that did not answer. Raised to
  400 with the reasoning beside the constant.
- `send.sh`'s default 600 ms idle threshold ends a capture before a later event.
  The first lease-expiry capture **exited 0 having never observed the expiry** —
  exit status answered "did HOLD work", not "did we see it". Retried under a new
  label with `--idle-seconds 8`; the failed attempt is kept.
- The four acceptance tools are mode 644 while their own usage text shows bare
  invocation, which fails. They have no shebang, so the usage text was corrected
  to `uv run python` rather than the files being made executable.

**Step 4 of the 2026-09-17 sequence was deliberately not run.** `LOGGER INTERVAL
60` would have reverted the cadence decision and cut storage headroom from about
16 days to about 3. `LOGGER INTERVAL 5` was sent while armed instead: refused,
`ERROR`, exit 1, cadence unchanged at 300 at both ends of the run.

## 2026-10-02: RTC-retained autonomous state extracted as the eighth module (D-063) — the build and software gate

CODE stage, run against HEAD `65f55d1` plus the uncommitted capture-gap work.
Software only: no upload, no serial port and no device command. Experiment 3 was
not touched. The boundary was chosen by the user from three options: move the
ten definitions and the magic, export them by `extern`, and leave every writer
and all of reservation in the sketch. See D-063.

**Files.** New: `Arduino/solar-logger/autonomous_retained_state.h`, `.cpp` and
`tests/test_characterization_autonomous_retained_state.py`. Changed:
`solar-logger.ino` (the definitions replaced by an `#include`, and three
comments that named the old location) and a comment-only edit in
`sequence_authority.h`. **No existing test file changed.**

**Red, then green.** The new file has 34 tests. Before the module existed, 32
failed and 2 passed:

- Real red evidence came from a second run, with the module files in place and
  the sketch's definitions not yet deleted. Three tests then failed by name:
  `solar-logger.ino defines rtcAutoMagic a second time`, `solar-logger.ino
  defines AUTO_RTC_MAGIC a second time`, and the writer-set test's
  `rtcAutoMagic is written outside a function body other than at its
  definition`.
- The 30 parametrized boundary guards are absence guards. They failed first
  only because the files did not exist, and they would pass against an empty
  module.
- `test_the_module_defines_no_function` and
  `test_only_the_known_functions_write_retained_state` passed before the move.
  The second is the compensating control for the `extern`, not evidence that
  the move happened.

| Gate | Result |
| --- | --- |
| Baseline `tools/check.sh` at `65f55d1` plus the capture-gap work | `ALL OK`, exit 0, **529 passed / 1 xfailed**, flash 1,106,273 B, globals 36,332 B, IntelliSense 8/8 |
| After the move, same tree | `ALL OK`, exit 0, **563 passed / 1 xfailed**, flash **1,106,273 B (unchanged)**, globals 36,332 B (unchanged), IntelliSense **9/9** |

The 34 added passes are the new file. Both gates ran on the combined working
tree; no isolated gate was run. The strict short-cadence overrun `xfail` is
still xfailed.

**Token audit (D-047).** The new sketch equals HEAD's sketch with one 66-token
run removed and `# include "autonomous_retained_state.h"` inserted, checked by
reconstruction rather than by a diff. The run's magic definition is verbatim in
the header, and its ten definitions are verbatim and in order in the `.cpp`.
`sequence_authority.h` is token-identical to HEAD. Sketch: 6,680 lines before,
6,670 after.

**Image comparison.** Both images are `--clean` builds, without the uploader's
revision injection; ELF copies were kept in session scratchpads, not in the
repository. All ten allocated `PROGBITS` sections keep their address and size,
and **exactly four bytes differ across all of them**: the core's compile-time
stamp in its "Software Info" string inside `.flash.rodata`. The two symbol
tables are identical — sorted `riscv32-esp-elf-nm -S` output diffs empty across
all 9,927 entries. **Measured, not predicted:** before the move the CODE stage
predicted a few bytes of flash change from cross-unit address loads. That was
wrong; the change is zero.

Verified independently by the orchestrator on 2026-10-02 from its own pair of
`--clean` builds, whose stamps were `11:19:42` against `13:10:27`. The CODE
stage reported `11:05:36` against `13:01:12` from its own builds, and reported
the symbol count as 7,900, which does not reproduce from `nm -S` (9,927 entries)
or from a defined code/data filter (7,603). The conclusion reproduces exactly;
only that one count did not, and it is corrected above.

**Audit ELFs, not `.bin` files.** The two `.bin` images differ in 69 bytes, in
three clusters, and only four of them are the ELF difference above. The other
65 are esptool's: a 32-byte `app_elf_sha256` patched into the application
descriptor, and a 32-byte image SHA-256 plus a checksum byte appended at the
tail. Both are hashes of the ELF, so neither can be inside it, and both change
whenever the stamp does. Recorded because a future audit diffing `.bin` files
will see 69 and should not hunt for 65 bytes of code.

**RTC layout.** All twelve project RTC symbols appear exactly once, in
`.rtc.data` (section 3), at the same addresses and sizes before and after:
`0x50000018`–`0x5000004F`, `rtcSleepTest*` last, the `int64_t` totals at
`0x50000020` and `0x50000028`, both 8-byte aligned. The open ESP-IDF v5.5.5
`should_load()` question (D-060) was not pursued.

**Kickoff correction.** The kickoff's writer table missed two writers:
`runAutonomousWakeCycle()` also writes `rtcAutoNextSeq` and `rtcAutoCycleCount`
by `++`. The orchestrator verified the correction. There are still nine writer
functions in total.

**Two orchestrator claims corrected by the CODE stage, both checked.**

- The `nm` symbol count of 7,900 *is* reproducible; the fault was not naming the
  command. It is `riscv32-esp-elf-nm -S --size-sort | wc -l`, which lists only
  symbols carrying a size, against 9,927 for plain `nm -S`. Confirmed on both
  ELFs. The record above keeps the identical-symbol-tables form because it
  states its own command.
- **The writer-set test survives a relocation, and the orchestrator was wrong to
  say the reservation stage would have to update it.** Settled by experiment
  rather than by reading, on the CODE stage's own suggestion: in a scratch copy
  of the sketch directory, `reserveSequenceBlock()` was cut verbatim out of
  `solar-logger.ino` into a new `sequence_reservation.cpp`, and the tree was
  re-read through `firmware_source`. The function was found at its new location,
  the mirror's writer set stayed `{autoStorageRecover, reserveSequenceBlock}`,
  and the file-scope-write check held, so the test passes unedited. Two negative
  cases confirm it is still a guard rather than vacuous: adding
  `sneakyExtraWriter()` produced a three-name set, and renaming the writer to
  `reserveSequenceBlockV2` produced the renamed set. Both correctly fail. The
  consequence is recorded in BACKLOG — the reservation stage gets no red test
  from this and needs its own positive evidence.

**Not changed, deliberately.** `stopAutonomousTest()` still leaves the running
totals alone (BACKLOG). One test docstring is now out of date and was left for
the orchestrator: `tests/test_characterization_sequence_authority.py` says the
mirror `rtcAutoSeqHighWater` itself was left in the sketch. Its assertions are
still true; only the mirror's definition moved.

**Hardware acceptance needed, not performed.** Retained state needs the full
2026-09-17 acceptance sequence, not only the extraction smoke (BACKLOG). The
image is byte-identical apart from the build stamp, so this checks the build and
upload, not new code paths. The transcript lines that would show the move kept
retained state working:

- **Retained lifetime across the wake boundary.** Step 8: five consecutive
  unclaimed timer wakes with the same `boot_id`, `[AUTO] Cycle:` increasing by
  one per wake, and record sequences contiguous. A broken retained definition
  would show up as `[AUTO] WARNING: Timer wake with no valid RTC session
  state.` on every wake, a new `boot_id` each time, and cycle 0 every time.
- **Session-clock continuity.** Step 8: `session_elapsed_ms` never decreases
  while `boot_id` is unchanged (D-033), each `interval_ms` is near the 300 s
  cadence, and `[AUTO] Commanded sleep for that interval:` is consistent with
  that cadence. Step 6's RELEASE on a timer wake and step 7's lease expiry
  must each be followed by a record that continues `session_elapsed_ms` rather
  than restarting near zero.
- **Power-test RTC state separate.** Shown at build level, not by the sequence:
  `rtcSleepTestMagic` and `rtcSleepTestCycle` keep their addresses, stay in the
  sketch and are absent from the module, which a test pins. Step 10 (`POWER TEST
  STOP` refused during a rendezvous, next record unaffected) shows only that the
  power-test path still leaves autonomous state alone. Nothing in the sequence
  arms a deep-sleep power test. `LOGGER AUTONOMOUS STATUS` on a rendezvous
  should print `[AUTO] RTC state valid: YES` and a `Cycles this power-on:` count
  that kept rising since the upload's cold boot.
- **Refusals.** Steps 9 and 10, as written.

## 2026-10-01: Sequence authority extracted as the seventh module (D-062) — NOT committed, uploaded or hardware validated

CODE stage, run against HEAD `6192c2d`. Software only: no upload, no serial
port and no device command. Experiment 3 was not touched. The boundary was
chosen by the user: move only the D-023 decision, and leave reservation and the
RTC mirror in the sketch. See D-062 for the reasoning.

**Files.** New: `Arduino/solar-logger/sequence_authority.h`, `.cpp` and
`tests/test_characterization_sequence_authority.py`. Changed:
`solar-logger.ino` (the decision replaced by a call, plus the include and
comments), comment-only edits in `storage.h` and `storage.cpp` that named the
old location, and one harness line in
`tests/test_characterization_storage_recovery.py`. That line adds
`sequenceAuthorityNext` to `EXTRACTED`, so the host build of
`autoStorageRecover()` links it; the docstring's function list gained a line
too. No assertion in an existing test changed.

**Red, then green.** Before the code existed, the new test file failed 30 of 30.
Only two of those are real red evidence: the decision was "defined 0 times",
and it had no caller. The other 28 are absence guards, and they failed only
because the module files did not exist yet. They pass on an empty module too,
which is why the two positive tests exist.

| Gate | Result |
| --- | --- |
| Baseline `tools/check.sh` at `6192c2d` plus others' uncommitted work | `ALL OK`, **499 passed / 1 xfailed**, flash 1,106,195 B, globals 36,332 B, IntelliSense 7/7 |
| After the move, same tree | `ALL OK`, **529 passed / 1 xfailed**, flash **1,106,273 B (+78)**, globals 36,332 B (unchanged), IntelliSense **8/8** |

The 30 added passes are the new file. Both gates ran on the combined working
tree. No isolated gate (`6192c2d` plus only this stage's files) was run.

**Image comparison.** Both images were built with `--clean` into a scratch
directory, without the uploader's revision injection. `.flash.text` +34 B:
`sequenceAuthorityNext` 244 B is new, and `autoStorageRecover(bool)` went from
522 to 312 B. No other symbol changed size. `.eh_frame` +44 B, with exactly one
new FDE (6,356 to 6,357). The 1,757 human-readable rodata strings are identical.
The raw rodata bytes differ only in address-valued bytes and one time-of-build
string, 13:11:53 versus 13:14:45. That string does not come from the project
sources, which contain no `__TIME__`.

**Token audit (D-047).** Five runs, 163 tokens, left the sketch. What came in
is the `#include`, the function name and its arguments `scan ,
rtcAutoSeqHighWater`. The module body matches the removed tokens except that
`rtcAutoSeqHighWater + 1` became `reservedHighWater + 1`, plus `return next`.
`logIntact` and `fromLog` are now computed after the silent
`loadSequenceHighWater()` instead of before it, which cannot change their
values or the output.

**Sketch:** 6,735 lines before, 6,680 after.

**Hardware acceptance needed, not performed.** This stage needs the normal
smoke after an extraction (BACKLOG "Hardware smoke test after an extraction
group"). The INFO path does not run this code. The cold-boot and arm paths do,
so check the boot transcript's `[STORAGE] Next sequence from log: …, from NVS
reservation: …, using: …` and the provably-intact line, and confirm the next
record continues the log with no gap. Damaged-log branches stay host-tested
only.

## 2026-10-01: Recovery is told whether the retained totals were valid (D-061) — NOT committed, uploaded or hardware validated

CODE stage. Software only: no upload, no serial port, no reset, clear, erase or
corruption injection. Experiment 3 untouched. Two files changed in the working
tree: `solar-logger.ino` (`autoStorageRecover()` and its four call sites) and
`tests/test_characterization_storage_recovery.py`. The kickoff reports that the
board runs `58a4e09` after a passed healthy-log acceptance today. That
acceptance is not recorded in these docs yet; this stage did not check it.

### The change

`autoStorageRecover()` now takes `bool retainedStateValid`. Each call site
captures `rtcAutoMagic == AUTO_RTC_MAGIC` into a local immediately before it
sets the magic, and passes that local in. When the scan could not read the log
(`openFailed || readError`):

- valid: the totals are carried, as under D-060, and a new line says the
  retained state was valid
- not valid: the totals are set to 0, and two lines say the retained state was
  not valid and that the totals restart at zero

Every readable log reseeds from its last record exactly as before. Policy: D-061.

**Why a parameter.** The validity exists only at the call site, for one
statement, before the magic is set. A parameter makes each of the four callers
state it, and the compiler rejects a caller that does not. Moving the magic
assignment after the call and reading the global inside recovery would have
worked too, but it would hide an ordering rule in four places. BACKLOG calls
that kind of latent ordering dependency the worst thing to carry across a file
split. The harness also needs no model of `rtcAutoMagic`: it passes the bool
from a new eighth argument, which defaults to invalid, as after a power-on
reset. The return type is still `uint32_t`, so the slice-by-name assertion is
unchanged.

### Is the cold-boot resume reachable with valid retained state? Yes, in two ways

Read from the source, not observed. The resume site in `setup()` is the
`else` branch after `autonomousColdBootMaintenanceWindow()`.

1. **A reset that is not a deep-sleep wake**, if RTC memory survives it. That
   is exactly the ESP-IDF `should_load()` question D-060 could not close. The
   firmware no longer needs the answer: valid carries and says so, invalid
   zeroes and says so.
2. **A timer wake whose first NVS load reads "not armed"**. `setup()` loads the
   armed flag twice, once in the timer-wake branch and again during ordinary
   initialization. `loadAutonomousConfig()` turns a namespace that will not
   open into "not armed" with no signal (BACKLOG, "NVS load failures become
   plausible defaults with no signal"). If the first load fails and the second
   succeeds, the timer branch is skipped with the magic still valid, and the
   resume runs. That needs a transient NVS failure and has never been seen.

It is reached with **invalid** state by a timer wake that lost RTC state and
then failed its storage mount, which falls through on purpose.

A timer wake claimed by a host does **not** reach the resume. The claim sets the
USB transport bit, `anyHostConnected()` reads only that bit, and only
`connectionRelease()` clears it. Nothing between the rendezvous return and the
resume calls `connectionRelease()`, so the "host session is held" branch is
taken. A wake disarmed during its rendezvous does not reach it either, because
`autonomousTestArmed` is false. A failed sleep (`esp_sleep_enable_timer_wakeup`
or `esp_deep_sleep_start` returning) returns from `setup()` before the resume.

### Behavior that changes from 58a4e09

Only when the log cannot be read. A healthy, repaired-tail or mid-file-damaged
log reseeds as before, and no new line is printed.

- **Timer-wake RTC-loss rebuild and handoff rebuild:** both sit inside
  `rtcAutoMagic != AUTO_RTC_MAGIC`, so they now zero instead of carrying. This is
  the case the kickoff named.
- **Arm (`LOGGER AUTONOMOUS ON`):** the magic is ordinarily 0 here, because
  `stopAutonomousTest()` clears it and so does the image initializer. A re-arm
  after an in-session `LOGGER AUTONOMOUS OFF` used to carry the previous
  session's totals under D-060. It now zeroes them and says so. The values are
  still in RTC memory, but the firmware's own guard has declared them invalid,
  and this stage follows the guard.
- **Cold-boot resume:** follows whatever the magic was, as above.

Sequence selection, D-023 and the NVS floor are unaffected. The next sequence
was identical in each valid/invalid pair below.

### Tests first, and the red-check

Written before the firmware change. They replace D-060's two retained-totals
tests:

- `test_an_unread_log_carries_valid_retained_totals_and_says_so`,
  parametrized `open-failure` / `short-read`
- `test_an_unread_log_zeroes_invalid_retained_totals_and_says_so`, same
  parametrization
- `test_a_readable_log_still_replaces_whatever_the_totals_held`, now seeded
  VALID, so an implementation that preferred valid retained totals over a
  readable log would fail it
- `test_every_recovery_call_is_told_whether_retained_state_was_valid`, a source
  test. The harness runs recovery, not its callers, and validity read after the
  magic is set is always true. So it pins capture, then set, then call, at each
  of exactly four sites.

Red-check: the new driver calls `autoStorageRecover(retainedValid)`, which
`58a4e09` cannot compile, and erroring is not failing. Staged in a detached
worktree at `58a4e09`: the new test file copied in, and only the definition
widened to an **unnamed** `bool`. An unnamed parameter cannot be read, so the
body under test is exactly `58a4e09`'s. The call sites were left unchanged; the
harness does not compile them. The firmware itself was unedited at the time
(`git diff HEAD -- Arduino/` empty). Result: **5 failed, 1 passed**.

- invalid, both cases: `(5555, 6666) == (0, 0)` failed. The defect.
- valid, both cases: failed only on the new validity line. The values were
  already carried, which is D-060's half that stays.
- source-order test: failed, because no call passes validity.
- healthy log: passed, as it must.

After the fix the storage file runs **88 passed** (85, minus the 3 replaced,
plus 6). The script was a scratch copy of the September 29 `redcheck.sh`
technique. It is not preserved in `logs/evidence/`, which this stage did not
touch.

Serial from the fixed harness, disposable fixtures, retained seeded 5555/6666:

```text
[STORAGE] Running totals could NOT be recovered: the log could not be opened, so none of it was read.
[STORAGE] Retained RTC state was valid before recovery, so its totals are the best figure available.
[STORAGE] Carrying the retained totals instead: Qsum_uAh=5555 Esum_uWh=6666. These were NOT checked against the log.
```

```text
[STORAGE] Running totals could NOT be recovered: a short read stopped the scan before the end of the log.
[STORAGE] Retained RTC state was NOT valid before recovery, so there are no totals to carry.
[STORAGE] Restarting the running totals at Qsum_uAh=0 Esum_uWh=0. The next record does NOT continue the log's totals.
```

The other two combinations print the same lines with the other reason.

### Gate

`tools/check.sh`, each `ALL OK`, exit 0, all seven steps PASS:

| Scope | pytest | Flash / globals |
| --- | --- | --- |
| Isolated baseline: detached worktree at clean `58a4e09` | 472 passed, 1 xfailed | 1,105,707 B / 36,332 B |
| Isolated fix: that worktree plus only the two changed files | **475 passed, 1 xfailed** | **1,106,195 B** / 36,332 B |
| Combined working tree before the change | 496 passed, 1 xfailed | 1,105,707 B / 36,332 B |
| Combined working tree after the change, including other stages' uncommitted work | 499 passed, 1 xfailed | 1,106,195 B / 36,332 B |

Flash rose 488 bytes, which covers the new branch and its three strings. It was
not broken down by symbol. Globals are unchanged. Pyright 0/0/0, IntelliSense
7/7. The worktree was removed afterwards.

### Hardware acceptance still needed

A healthy-log smoke only. After upload, boot recovery must still reseed the
totals from the last record and print neither "could NOT be recovered" nor
"Retained RTC state". New records' Qsum/Esum must continue from the preceding
record. Both unreadable branches are host-injected only, which does not show
what a physical LittleFS open or read fault does. One must not be produced by
damaging the Experiment 3 log.

## 2026-09-29: D-060 reviewed and committed as 58a4e09 — one OPEN issue found, one citation corrected

Orchestrator review and close of the D-060 CODE stage recorded in the entry
below. No board was touched. The board still runs `1980d94` and has now not run
two commits, `44d8b27` and `58a4e09`.

### Gate re-run on the working tree

`tools/check.sh` exit 0, `ALL OK`, all seven steps PASS. pytest **496 passed,
1 xfailed** in 55.87 s; pyright 0/0/0; warning-free `--clean` compile;
IntelliSense 7/7. Flash **1,105,707 B** (84%), globals **36,332 B** (11%). Every
figure the CODE stage reported reproduces here, including the **+434** flash
delta over `44d8b27`.

### Verified against the source, not the report

- **`Print` really does take `long long`.** The new message casts the totals to
  `long long`, and a warning-free compile would not distinguish an integer
  overload from a silent promotion to `double`. Core 3.3.11's `Print.h` declares
  `size_t print(long long, int = DEC)` at line 91, so the integer overload is
  what binds. Checked in the installed header, not from memory.
- **The CODE stage's caveat about the cold-boot resume is correct.** Line 6337
  sets `rtcAutoMagic = AUTO_RTC_MAGIC` unconditionally and then calls recovery,
  with no prior test of the magic. The kickoff had asserted that all four sites
  setting the magic meant recovery only ran when retained state was invalid.
  That inference does not follow and was wrong in the kickoff; the CODE stage
  caught it.

### Citation corrected: the ESP-IDF version does not match the toolchain

D-060 cited `should_load()` "read from the v5.4 source". The installed core
3.3.11 bundles **ESP-IDF v5.5.5**, pinned from
`~/Library/Arduino15/packages/esp32/tools/esp32c3-libs/3.3.11/include/esp_common/include/esp_idf_version.h`
(MAJOR 5, MINOR 5, PATCH 5). Only headers and prebuilt libraries are installed
locally, so v5.5.5's `should_load()` has not been read by anyone. D-060 now
names the gap instead of implying the versions match. The zero-after-reset
behavior is EXPECTED, not measured, and has never been observed on this board.

### OPEN issue found in review

`rtcAutoMagic` is the firmware's own answer to "is retained state valid", which
D-011 introduced because RTC memory is undefined after a power-on reset.
Recovery cannot read it: all four call sites overwrite it immediately before
calling. Two of them reach recovery *because* it was invalid, and one says in
its own comment not to guess the running totals on that path — which is exactly
what `58a4e09` now carries there when the log is unreadable.

Bounded, not silent: the new message prints the carried value and says it was
not checked against the log. Filed as its own BACKLOG item rather than absorbed
into this stage, together with the unproven reachability of the line 6337 site.

### Commit

`58a4e09`, two files, +102/-4, on main, **not pushed**. Documentation remains
uncommitted, still interleaved with other stages' prose.

### Still outstanding

The healthy-log hardware smoke, now covering two unvalidated commits rather than
one.

## 2026-09-29: Running totals are no longer reseeded from an unread log (D-060) — NOT committed, uploaded or hardware validated

CODE stage. Software only: no upload, no serial port, no reset, clear, erase or
corruption injection. Experiment 3 untouched; the board still runs `1980d94`.
Two files changed in the working tree: `solar-logger.ino`
(`autoStorageRecover()` only) and `tests/test_characterization_storage_recovery.py`.

### The change

When `scan.openFailed || scan.readError`, `autoStorageRecover()` no longer
assigns `rtcAutoRunChargeUAh` / `rtcAutoRunEnergyUWh`. It prints why and what it
carries instead. Every other outcome reseeds as before. Policy: D-060.

### Tests first, and the red-check

Three tests, written before the firmware change. The harness gained one optional
argument pair that seeds the retained totals before recovery. Without it the
failed-open case cannot be tested on values, because "left alone" and
"overwritten with the scan's zero" both give 0/0 from a zero start. It also
gained a `print(long long)` overload on the recording Serial, matching the
sketch's `Serial.print(static_cast<long long>(...))` idiom.

- `test_an_unread_log_leaves_the_retained_running_totals_alone_and_says_so`,
  parametrized `open-failure` (`deny_log_open`) and `short-read`
  (`phantomBytes=72`); retained seeded to 5555/6666, log records 100/200 then
  300/400.
- `test_a_readable_log_still_replaces_whatever_the_totals_held`: the same log,
  read cleanly, still reseeds 300/400 over the seeded value and prints no
  recovery-failure line.

Red-check: no struct member was added, so the new tests compile against the
unmodified `44d8b27` sketch directly and no staging was needed. Run in the main
tree before the sketch was edited (`git diff HEAD -- Arduino/` empty): **both
failure-mode cases FAILED**, `(0, 0) == (5555, 6666)` for the failed open and
`(300, 400) == (5555, 6666)` for the short read, the stale last-reached record.
The healthy-log test and the two existing reseed tests
(`test_a_valid_log_reseeds_the_running_totals_from_its_last_record`,
`test_running_totals_are_reseeded_past_the_damage`) passed, and are unedited.
After the fix the storage file runs **85 passed** (82 before plus 3).

Serial from the fixed harness, disposable fixture:

```text
[STORAGE] Running totals could NOT be recovered: the log could not be opened, so none of it was read.
[STORAGE] Carrying the retained totals instead: Qsum_uAh=5555 Esum_uWh=6666. These were NOT checked against the log.
```

```text
[STORAGE] Running totals could NOT be recovered: a short read stopped the scan before the end of the log.
[STORAGE] Carrying the retained totals instead: Qsum_uAh=5555 Esum_uWh=6666. These were NOT checked against the log.
```

The healthy log's recovery serial has no new line.

### Which deployed paths actually change

All four call sites (`beginAutonomousSleepFromHostSession` rebuild, arm, the
timer-wake RTC-loss rebuild and the cold-boot resume) set `rtcAutoMagic`
immediately before calling recovery. The first and third are inside an explicit
`rtcAutoMagic != AUTO_RTC_MAGIC` test. The arm path runs with the magic 0 (set
by `stopAutonomousTest()` or the image initializer). **The cold-boot resume
does not test the magic.** A timer wake reaches it after an earlier error. I
did not prove it unreachable with valid retained state; if it is reachable, the
new behavior carries live totals there rather than overwriting them.

- Any reset that is not a deep-sleep wake: the totals are the image initializer,
  0. That is verified against the bootloader source (D-060). Open failure: the
  value is unchanged at 0, and the two lines are new. Short read: the value
  changes from the stale last-reached record to 0, plus the two lines.
- Re-arm after an in-session `LOGGER AUTONOMOUS OFF`, or a timer wake with an
  invalid magic: the previous session's totals are carried and printed, instead
  of 0 or a stale partial value.
- A healthy, repaired-tail or mid-file-damaged log: unchanged.

Nothing here reached the records' format, the CRC, D-023 sequence authority or
the NVS floor, and the firmware identity is unchanged.

### Gate

`tools/check.sh`, three scopes, each `ALL OK`, exit 0, every step green:

| Scope | pytest | Flash / globals |
| --- | --- | --- |
| Isolated baseline: detached worktree at clean `44d8b27` | 469 passed, 1 xfailed | 1,105,273 B / 36,332 B |
| Isolated fix: that worktree plus only the two changed files | **472 passed, 1 xfailed** | **1,105,707 B** / 36,332 B |
| Combined working tree after the change, including other stages' uncommitted work | 496 passed, 1 xfailed | 1,105,707 B / 36,332 B |

Flash rose 434 bytes. That covers the new branch and its three message strings;
it was not broken down by symbol. Globals are unchanged. IntelliSense 7/7,
pyright 0 errors, 0 warnings, 0 informations. A working-tree gate also ran
before the change and exited 0 with pytest 493 passed / 1 xfailed. Its later
steps overlapped the start of these edits, so only its pytest count is a clean
pre-change figure, and the isolated baseline above is the clean one. Both
worktrees were removed afterwards.

### Hardware acceptance still needed

A healthy-log smoke only: after upload, boot recovery must still reseed the
totals from the last record and print no "could NOT be recovered" line, and new
records' Qsum/Esum must continue from the preceding record. The failure branches
are host-injected only. That does not show what a physical LittleFS open or
read fault does, and one must not be produced by damaging the Experiment 3 log.

## 2026-09-29: Scan-open fix reviewed, red-checked and committed as 44d8b27 — still NOT hardware validated

Orchestrator close of the CODE stage recorded in the entry below. No board was
touched: no upload, no serial port, no reset or storage clear. Experiment 3 is
untouched, and the uploaded image is still `1980d94`, which HAS the defect.

### Gate re-run on the working tree

`tools/check.sh` exit 0, `ALL OK`. pytest **493 passed, 1 xfailed** in 51.58 s;
pyright 0 errors, 0 warnings, 0 informations; py_compile; warning-free `--clean`
compile; IntelliSense **7/7**; shell syntax; whitespace. Flash **1,105,273 B**
(84%), globals **36,332 B** (11%). Every figure reproduces the CODE session's,
so what that report listed as predicted is now observed.

The IntelliSense step printed two stale-directory notices, for
`.vscode/arduino-build` and the old-layout contents of `build/intellisense`.
Both are dead, neither is a failure, and no cleanup was performed.

### Red-check — the CODE report's form of it does not reproduce

That report claimed 5 of the 7 new tests fail against pre-fix code, without
saying how it was staged. **Run directly, it does not compile**: `reportScan()`
reads `scan.openFailed`, which `1980d94`'s `AutoLogScan` has no member for, so
all seven ERROR at fixture setup instead of failing.

Staged instead in a detached worktree at `1980d94` with the new test file copied
in and **only the `openFailed` field declared** in `storage.h` and never set, so
the behavior under test stays pre-fix. `AutoLogScan scan = {}` value-initializes
it to false, so the staging is deterministic. Result: **5 failed, 2 passed**.
The 2 that pass are the conservative half the entry below already recorded —
the file is untouched and the NVS floor is applied.

The second defect reproduced verbatim on that pre-fix tree, which is the
measured form of the argument made in the entry below:

```text
[STORAGE] ERROR: Could not open the log for scanning.
[STORAGE] Rewriting the log to its valid prefix: 0 of 0 bytes.
[STORAGE] ERROR: Could not open the log for recovery.
```

`autoStorageTruncateToValid()` announced the rewrite and proceeded; only its own
source open failing, for the same reason the scan's had, stopped it.

The worktree was removed afterwards and the working tree verified unchanged.

### Review

Every consumer of `readError` was enumerated, because setting it on an open
failure changes each one. The rewrite's own guard in `storage.cpp`
(`scan.validAfterInvalidRecords > 0 || scan.readError`) now refuses; `logIntact`
in `autoStorageRecover()` drops to the NVS floor, which the `nextSeq == 5001`
test pins; `autoStorageScanAndRepair()`'s `else if (scan.readError)` is ordered
behind the new `openFailed` branch; and `printStorageInfo()`'s `readError`
branch is unreachable for this case because of the early return above it. Those
are all of them in the firmware.

### Commit

`44d8b27`, four files, +296/-17, on main, **not pushed**. The documentation
describing the fix was deliberately left out, because it is interleaved with the
capture-gap stage's prose in the same files, and it remains uncommitted.

### Still outstanding

The healthy-log hardware smoke, which is the only check that matters for this
change: it must not have moved the path it does not touch. The open-failure
branch needs a real filesystem fault and must not be obtained by damaging the
Experiment 3 log.

## 2026-09-29: An unread log no longer reports an intact tail — NOT hardware validated

A bounded correctness fix to the open-for-scan diagnostic recorded in BACKLOG
on September 29. Nothing was uploaded, no serial port was opened and no board
was touched. Experiment 3 was not reset, cleared or read. Every failure in this
work is staged in a pytest temporary directory.

**This is not a hardware result.** The image was compiled and the host tests
were run. A physical open failure has never been observed on this board, and
the harness produces one by refusing the read-open while leaving the file
untouched — software failure injection, which does not establish what a real
LittleFS fault does.

### The defect, as the code had it

`autoStorageScan()` printed `[STORAGE] ERROR: Could not open the log for
scanning.` and returned the zero-initialized `AutoLogScan` with `readError`
still false. Every count in it was zero because nothing had been read, and both
callers read those zeros as measurements:

| Caller | What it said about a log it had never opened |
| --- | --- |
| `autoStorageScanAndRepair()` | `File bytes: 0`, `Valid records: 0`, then `Log tail is intact: ALL OK` |
| `printStorageInfo()` | `Log bytes: 0`, `Valid records: 0`, `Trailing bytes: 0`, `Tail status: INTACT`, and `CMD_RESULT,LOGGER STORAGE INFO,OK` |

A readable empty log produces exactly those numbers and that tail status, and
truthfully. The two were indistinguishable to a caller and to an operator.

The conservative half was real and is unchanged: `logIntact` already required
`validRecords > 0`, so the NVS reservation floor applied, and `trailingBytes`
being zero meant `autoStorageScanAndRepair()` never entered its repair branch.
No log bytes were at risk through the normal path.

### A second defect, found by the regression test rather than by reading

Driven **directly** against an unopened log — which is what the `force-truncate`
harness command exists to do — `autoStorageTruncateToValid()` printed

```text
[STORAGE] Rewriting the log to its valid prefix: 0 of 0 bytes.
[STORAGE] ERROR: Could not open the log for recovery.
```

It announced the rewrite and proceeded. The only thing that stopped it was its
own source open failing for the same reason the scan's had. Had the fault
cleared in between, it would have copied `keepBytes` — zero — into the temporary
file, removed the log and renamed the empty file over it, destroying every
record on the strength of a scan that read nothing.

`autoStorageScanAndRepair()` cannot reach that branch for an unopened log,
because `else if (scan.trailingBytes > 0)` is false. That is the caller's
`else if` chain, not this function's own guarantee, and it is precisely the
argument D-058 made for keeping the refusal compiled in the function instead of
letting the compiler infer it from the one call site.

### What changed

`AutoLogScan` gains one field, `openFailed`, and a failed read-open sets it
together with `readError`.

- **`readError` is what carries the case into the existing refusals.** The
  rewrite's guard is `validAfterInvalidRecords > 0 || scan.readError`, and
  `logIntact` includes `!readError`. Setting it is what makes those cover an
  unopened log without either of them learning a new condition.
- **`openFailed` carries what `readError` cannot.** A short read has real
  partial counts; a failed open has none. The distinction decides whether a
  number may be printed at all.
- `autoStorageScanAndRepair()` reports the size, record count and sequence range
  as UNKNOWN rather than zero, and names the tail **UNREAD** — neither intact
  nor damaged — before the read-error branch, whose "could not be read to the
  end" understates this and whose advice to run `LOGGER STORAGE DUMP` would open
  the same file and fail the same way.
- `printStorageInfo()` prints `Tail status: UNREADABLE`, prints no figure it did
  not measure, and **returns false**, so the command answers
  `CMD_RESULT,...,ERROR`. Under D-041 a command that did not do what was asked
  does not report `OK`, and the mount failure directly above it already reported
  itself that way.
- D-023 authority, the healthy-log path and the record format are untouched.

What an operator now sees for a log that will not open:

```text
[STORAGE] ERROR: Could not open the log for scanning. The log exists and none of it was read, so nothing is known about its contents and nothing will be repaired automatically.
[STORAGE] Scanning durable log...
[STORAGE] The log exists but could not be opened. Its size, record count and sequence range are UNKNOWN, not zero.
[STORAGE] The log was never read, so its tail is neither intact nor damaged: it is UNREAD. NOTHING WAS DISCARDED, and nothing will be repaired automatically while this persists.
[STORAGE] Appending continues normally, and the NVS reservation floor decides the next sequence because the log proved nothing.
[STORAGE] Next sequence from log: 1, from NVS reservation: 5001, using: 5001
[STORAGE] Log tail is NOT provably intact, so the NVS reservation floor applies.
```

### Tests

Seven added to `tests/test_characterization_storage_recovery.py`, all executing
the firmware's own compiled functions. **No existing test was changed**; the
harness gained one staging control beside the existing phantom-bytes one, which
fails read-opens of the log and leaves the file alone, so a test can prove the
bytes were never touched.

**Five of the seven fail against the code they replace**, and the two that pass
are the conservative half above — the log is left alone, and the NVS floor
applies — which is exactly what the BACKLOG finding said was already true and is
worth pinning so a later change cannot quietly lose it.

| Test | Pre-fix |
| --- | --- |
| the scan reports it unreadable, not scanned | FAIL |
| it is never called intact | FAIL |
| it is left exactly as it was | pass |
| it keeps the NVS reservation floor | pass |
| INFO reports UNREADABLE and answers ERROR | FAIL |
| absent, empty and unopenable are three answers | FAIL |
| the rewrite refuses it even when called directly | FAIL |

### Gates

Both run in full, with their scopes stated rather than mixed:

| Gate | Scope | Result |
| --- | --- | --- |
| Before, working tree | the tree as found, including the uncommitted host capture-gap work | `ALL OK` — **486 passed, 1 xfailed** |
| After, working tree | the same tree plus this change | `ALL OK` — **493 passed, 1 xfailed** |
| After, isolated snapshot | `1980d94` plus only the four files this change touches, with the host tool, its tests and the pandas dependency excluded | `ALL OK` — **469 passed, 1 xfailed** |

469 is 462 plus these seven, against the September 29 isolated storage baseline.
493 is 486 plus the same seven. pyright, py_compile, the warning-free clean
Arduino compile, IntelliSense 7/7, shell syntax and tracked whitespace passed in
both. The strict `xfail` is the unrelated short-cadence autonomous overrun
defect; it was not touched. No dependency or tooling change was made.

### Flash, accounted for rather than accepted (D-047)

Program storage **1,104,313 → 1,105,273 bytes, +960**. Global variables
**36,332, unchanged**. Both images were rebuilt clean into separate output
directories and the baseline reproduced `HEAD` exactly.

| Section | Before | After | Delta | What it is |
| --- | ---: | ---: | ---: | --- |
| `.flash.text` | 826,966 | 827,082 | **+116** | three symbols, and only three |
| `.flash.rodata` | 168,508 | 169,348 | **+840** | the new messages |
| `.eh_frame` | 30,316 | 30,320 | **+4** | one existing FDE |
| | | | **+960** | |

Every other section is byte-identical, including all four `.dram0`/`.noinit`
sections that make up the globals figure.

**The 116 code bytes are the three functions this change edits, and nothing
else.** A per-symbol comparison of the two ELFs finds exactly three that changed
size: `printStorageInfo()` +58, `autoStorageScanAndRepair()` +52,
`autoStorageScan()` +6. No symbol was added or removed, which the identical FDE
count of **6,356** confirms independently, so the `.eh_frame` +4 is one existing
FDE whose CFI grew with the body it describes.

`autoStorageTruncateToValid()` is **0x266 = 614 bytes and type `T` in both
images**, the same figure D-058 recorded. Its refusal is still compiled, which
is measured here rather than reviewed, as D-058 requires.

**The 840 rodata bytes are message text.** Comparing the two images'
NUL-terminated string tables as multisets: 908 bytes of strings arrived, 74 left,
net **834**. The remaining 6 are packing in the merged string pool. Of the
arrivals, 9 bytes are the core's build timestamp and not this change's. One
departure needs saying: the bare `"UNREADABLE"` literal disappears from the
table because the linker now stores it as the suffix of
`"[STORAGE] Tail status:      UNREADABLE"`. It still occurs exactly once as
`UNREADABLE\0` in both images, so the short-read branch that prints it did not
lose its string.

### Found and not fixed

`autoStorageRecover()` still ends with
`rtcAutoRunChargeUAh = scan.lastRunChargeUAh` and its energy equivalent. On this
outcome those are zero, so the RTC-retained running totals silently restart at
zero for a log nothing read, and nothing says so. It is a silent deviation
rather than a false report, and the same thing happens on the pre-existing
short-read path where at least a prefix was read. Changing it alters deployed,
characterized accounting behavior on both paths, so it is recorded in BACKLOG as
its own stage rather than carried inside a diagnostic fix. These are the
test-local totals D-017 keeps separate from experiment state; no stored record
is affected.

Two smaller observations, neither changed: `autoStorageScan()`'s error line
prints **before** `autoStorageScanAndRepair()`'s "Scanning durable log..."
banner, because the scan runs before the banner; and `dumpStorage()` still
returns true after breaking on a short read mid-dump, having printed the error.
The first is cosmetic ordering, the second is a separate completion-semantics
question from the one fixed here.

### Ownership and what was left alone

Firmware, tests and the four files above are this stage's. The uncommitted host
capture-gap work (`tools/charger-transitions.py`,
`tests/test_charger_transitions.py`) and the pandas dependency changes were not
touched, and the isolated gate deliberately excludes them so the firmware figure
is not a mixed snapshot. The sealed September 29 hardware evidence was not
altered. Nothing was committed, staged or pushed. Firmware identity is unchanged
at `0.3.0-dev` / `solar-logger-protocol-ack-v3`; the record format stays version
1 at 72 bytes.

**Hardware acceptance, PENDING — nothing below has been run.** The healthy-log
path is the check this change most needs, because it is the path it must not
have moved: upload, then confirm `LOGGER STORAGE INFO` still reports the
Experiment 3 log with `Tail status: INTACT`, zero trailing bytes and at least as
many records as before, and that `LOGGER STORAGE DUMP` still decodes them with
invalid 0. The open-failure branch cannot be exercised without a real filesystem
fault, and **must not** be exercised by damaging the Experiment 3 log. If an
unexpected `UNREADABLE` or any storage error appears during that smoke, stop
acceptance and diagnose rather than continuing.

## 2026-09-29: Storage 1980d94 passes healthy-log hardware smoke

This session performed the deliberate hardware stage, following the startup
handoff and canonical docs. Git stayed on main at **1980d94**, empty index,
clean firmware/tools; source hashes remained unchanged. Existing host analysis,
dependencies and older docs/evidence remain uncommitted. App inventory found no
other active BMW chat, but process inspection found two Claude Code processes
with this repo as cwd; the user explicitly confirmed those CODE sessions idle.
No host logger or pending serial/upload marker was present. Initial sandboxed
process inspection was denied; the permitted escalated read established ownership.

**Fresh healthy-log storage hardware smoke passed, 2026-09-29, on clean
1980d94** (`0.3.0-dev`, `solar-logger-protocol-ack-v3`). Experiment 3 retained;
full pre/post dumps preserve all 10,659 preflight record lines exactly, and
post-upload boot 36 adds 3 records, 12073–12075. Final dump: 10,664 records,
1412–12075, board invalid 0; observed INFO scans have trailing 0 and INTACT tails
with no storage errors. [Raw evidence, checksums and limits](../logs/evidence/2026-09-29/storage-hardware/README.md).
Damaged-log branches remain host/synthetic tested only. No standalone reset,
storage clear, whole-flash erase or corruption injection was performed.


### Preflight and upload

Read-only VERSION reported **eba3b5d-dirty / 0.3.0-dev /
solar-logger-protocol-ack-v3**. NVS-backed autonomous status reported Experiment 3,
boot 35, armed YES, cadence 60 s. INFO at 09:00 PDT: 10,650 valid records,
766,800 bytes, 1412–12061, record size 72, trailing 0, INTACT. The complete
09:09 dump held 10,659 records through 12070; all exp=3, consecutive, invalid 0.
All 5,756 September 25 record lines matched exactly. BEGIN/END, count summary,
ACK and matching RESULT OK were verified before upload. The port disappeared
only after completion and normal unclaimed-rendezvous sleep. The sender's generic
“expected for RELEASE” stop text also appears for this DUMP; no RELEASE was sent
in preflight. Decoded output is preserved byte-for-byte, not a binary flash image.

Canonical `tools/upload.sh` ran only after that evidence was sealed. It compiled
clean revision 1980d94 and uploaded successfully on attempt 1 at 09:11 PDT.
Post-upload VERSION independently reported **1980d94**, with the unchanged version
and build ID. The injected build used 1,104,265 flash bytes and 36,332 global
bytes; this is distinct from the prior uninjected check build's 1,104,313 bytes.
Uploader performed its normal bootloader reset, targeted firmware-region erase/
program and restart; no separate reset, NVS/LittleFS erase or whole-flash erase
was requested. The upload log records all programmed addresses.

### Retention, session handoff and autonomous growth

| Observation | Valid/read count | Newest sequence | Notes |
| --- | ---: | ---: | --- |
| Preflight dump | 10,659 | 12070 | all exp=3, invalid 0 |
| Post-upload INFO | 10,661 | 12072 | 767,592 B; 72-byte records; trailing 0; INTACT |
| After session RELEASE, next autonomous wake INFO | 10,663 | 12074 | 767,736 B; trailing 0; INTACT |
| Final complete dump | 10,664 | 12075 | all exp=3; invalid 0; exact preflight prefix |

The old image added 12071/12072 before upload. New boot **36** starts at
**12073**, flags **0x4 FIRST_AFTER_BOOT**, then advances through **12075** with
flags **0x0**. All 3 new-boot records have interval_ms 60004. Counts describe
successive captures, not loss or contradictory summaries. Sequence numbers are
unique/consecutive here; this does not remove D-023's permitted reservation gaps.
Record version 1/72-byte contract is unchanged in the clean source; hardware INFO
reports 72 and DUMP validates records. Binary CRCs were not recomputed from rounded
text; invalid 0 is the board validator's report.

HOLD, KEEPALIVE, STATUS, SESSION STATUS and RELEASE each returned matching ACK and
RESULT OK. HOLD restored **Experiment 3, interval 479**, charge 86.901199 mAh,
energy 1248.362325 mWh; held STATUS agreed. RELEASE closed a **3.788 s** tethered
interval, saved checkpoint **480**, and resumed armed autonomous sleep with
**59,750 ms** remaining. New autonomous records appeared after that release.
The final dump ended after RESULT OK and the normal autonomous return to sleep.
No Python logger was started; no held lease remains from this test.

### Diagnostics and evidence limits

- **General STATUS during an unclaimed timer wake reported zeros**, including
  Experiment ID 0. Source confirms `printStatus()` reads experiment globals before
  the later `loadCheckpoint()` path restores them; NVS-backed AUTONOMOUS STATUS
  and every record say exp=3. Held STATUS correctly reports 3. This is a pre-existing
  diagnostic defect, not lost experiment state or an extraction regression; track
  a separate correction in BACKLOG. Do not rely on generic STATUS at that phase.
- Immediate post-upload AUTONOMOUS STATUS ran in the cold-boot maintenance window:
  armed YES but running NO, boot/next seq 0, RTC invalid. Later boot-36 records
  establish initialization and actual autonomous progress; those early zeros do
  not identify a failed recovery.
- HOLD and RELEASE each printed accumulator-reset readback **ENERGY=0 CHARGE=-1**.
  The existing sensor code warns on nonzero readback and returns success after
  readable registers; continuous conversion is a possible explanation, not a
  measured cause. These are retained sensor observations, not storage errors or
  induced-overflow/accumulator-reset proof.
- No storage error, invalid record, tail repair or corruption was observed.
  Torn/invalid-tail repair, mid-file/read-error refusal, the known open-for-scan
  diagnostic defect, storage-full behavior and crash/lease-expiry paths were
  not exercised by this healthy-log smoke. No physical fault was injected.
- All dumped epochs remain 0/time UNKNOWN. Retained elapsed values are not exact
  wall-clock coverage. The canonical sender normalizes serial lines and may
  discard pre-ACK chatter; this is not a complete raw serial/boot recording.

No firmware/source/test/dependency edit, commit or push was made. Documentation
and dated evidence were updated; no unnecessary unit tests/full software gate
were rerun for prose. Prior isolated 462/1 and combined 486/1 gates remain valid
within their recorded scopes. Fresh checks here: hardware protocol/completeness,
full decoded-prefix comparison, source hashes, checksum manifests and whitespace.

## 2026-09-29: Storage extraction reviewed and committed separately — hardware pending

The storage-only change was reviewed against **43c3baf** and committed separately
from capture-gap analysis, its pandas dependency changes, and older documentation/
evidence work. No firmware behavior was edited during this review. D-058 records
the boundary; this entry and D-058 accompany the four storage source/test files
in the commit. The other working-tree changes remain uncommitted and unpushed.

**Fresh move audit:** the bodies of autoStorageMount, autoStorageScan,
autoStorageTruncateToValid and autoStorageAppend, and the AutoLogScan layout, are
token-identical to the parent. Recombining autoStorageScanAndRepair with the
remaining autoStorageRecover body reproduces the original recovery logic exactly.
All 72 other old function bodies outside the five adapted callers are unchanged
(including moved functions). Expanding the new filesystem/timing accessors in
INFO, DUMP, CLEAR and the wake cycle also reproduces their original bodies.
Sequence authority, RTC state, scheduler, record bytes/CRC and command policy
remain in their prior owners. No newly introduced behavioral regression was found.

**Fresh isolated gate:** a temporary snapshot of parent 43c3baf plus only the
four storage source/test files ran the complete tools/check.sh: **exit 0, ALL OK;
462 passed, 1 xfailed**, pyright/py_compile clean, warning-free clean firmware
compile, IntelliSense **7/7**, shell syntax and whitespace passing. Flash is
**1,104,313 bytes**, globals **36,332 bytes**. The snapshot excludes the 24 host
analysis tests and pandas additions, so 462/1 is the isolated storage result;
486/1 remains the September 28 combined-working-tree gate. Its code bytes were
checked against the staged candidate. The known short-cadence overrun xfail
remains open. No extra tests or dependency changes were introduced in this review.

**Pre-existing diagnostic finding, not an extraction regression:** when the log
exists but open-for-scan fails, autoStorageScan prints an error and returns
without setting readError. Zero-initialized trailingBytes then lets recovery/
INFO print an INTACT tail despite the failed scan. The same body exists in
43c3baf. Sequence recovery still applies the NVS floor because validRecords is
zero, and no automatic tail repair runs on this outcome. Track explicit unreadable
scan status and an open-failure fixture as a separate correctness stage; do not
interpret an INTACT line as sufficient if any storage error was printed.

[Move audit and isolated gate](../logs/evidence/2026-09-29/README.md). No upload or serial
command was run. Experiment 3 was not reset or cleared. Storage hardware smoke
is deliberately the next session's bounded task: capture current VERSION and
healthy storage/experiment evidence before upload, use canonical tools/upload.sh,
and verify historical decoding plus new autonomous records afterward. Do not
inject corruption or begin another firmware extraction during that smoke.

## 2026-09-28: Capture-gap and initial-state reporting completed — host only

The user authorized continuation after the pending CODE-agent ownership question.
Fresh Git/source checks still showed main at **43c3baf**, nothing staged, and the
original positional-index fix only. Source/docs matched the September 25 baseline
apart from this task's checkpoint addendum; telemetry had grown. The previously
tested scratch draft was reviewed, applied to `tools/charger-transitions.py`
and tested in the canonical repo with `tests/test_charger_transitions.py`.
Storage/firmware/dependencies were preserved byte-for-byte against that baseline.
Nothing was committed, uploaded, reset or cleared; no serial command was issued.

The tool now reports initial state, post-gap state and contiguous edges separately
(D-059). Every capture segment is printed; gaps, experiment changes and repeated/
backward host timestamps restart persistence. Five classified samples per state
and a configurable 2 s inclusive maximum spacing are provisional analysis defaults.
Ambiguous rows break persistence; stale confirmed state cannot qualify a timed
edge. First supporting sample and fifth-sample confirmation are reported
separately, as is the adjacent host-sample bracket for a contiguous edge.
Input errors fail explicitly; context is clipped to the capture segment.

**Fresh CSV analysis:** 26,674 rows, all exp=3, from September 16
10:07:44.231779 to September 27 22:21:22.306909 (UTC-07:00). At defaults:
**81 segments / 80 gaps**, **2 contiguous transitions**, and **39 state
observations without a timed edge** (one initial, 38 after gaps). There are
46 UNKNOWN and 1,131 TRANSITION samples. Positive timestamp-spacing median is
1.000138 s. These counts describe captured evidence under the selected rule,
not physical charging-state truth or uninterrupted measurement coverage.

| Edge | Last preceding host sample | First supporting host sample | Confirmed at |
| --- | --- | --- | --- |
| ON-like | Sep 24 14:09:49.468777, -0.628 mA | 14:09:50.468071, +178.692 mA | 14:09:54.468872 |
| OFF-like | Sep 24 14:48:16.097899, +62.708 mA | 14:48:17.099301, -0.624 mA | 14:48:21.103139 |

These preserve the previously documented two contiguous edges; they do not add
LED correlation or identify causes. The old five-entry report conflated one
initial and two post-gap observations with those two edges. The corrected
report also exposes capture gaps where the observed state did not change.

**New capture since the earlier audit:** 77 morning rows on September 25,
08:27:12.636615–08:28:28.636627, follow a 49,337.521779 s gap. A further 165
rows on September 27, 22:18:38.306874–22:21:22.306909, follow a 222,609.670247 s
gap. Later capture does not establish overnight/weekend continuity or alter the
user's earlier statement that the host logger was stopped overnight.

**Validation actually run:** fresh `tools/check.sh` exit 0, **ALL OK** —
**486 passed, 1 xfailed**, pyright and py_compile clean, warning-free clean
Arduino compile, IntelliSense **7/7**, shell syntax and tracked whitespace pass.
The 24 new tests cover the reporting behavior with synthetic data and no device.
The existing short-cadence overrun is still the strict xfail. Flash is
**1,104,313 bytes**, globals **36,332 bytes**; storage is still unuploaded and
not hardware validated. The clean compile is not new board evidence.

Failures were not omitted: the first gate could not find `uv`/`uvx` on this
session's PATH (three Python steps exited 127; other steps passed). The installed
`/Users/afxjzs/.local/bin` was added for the rerun. A targeted type check then
found a nullable candidate-state annotation and an invariant list annotation in
the new test helper; using the current classified string and a read-only Sequence
resolved both before the final full gate. No dependency/tooling changes were made.

[Preserved report, final gate and input fingerprint](../logs/evidence/2026-09-28/README.md).
Next bounded stage: review/commit the existing storage extraction separately
from this host-analysis change, then a deliberate preservation-safe hardware
smoke. Exact board VERSION must be established then; Experiment 3 remains active.

## 2026-09-25: Full dump preserved; hardware identity and session continuity repaired

The user supplied the completed storage dump and the INA228 purchase-listing
screenshot. Originals are now preserved under
[evidence/2026-09-25/](../logs/evidence/2026-09-25/README.md), with SHA-256 checksums.
Temporary attachment paths are no longer the only source. This was documentation
and offline text validation only: no upload, serial command, code change, reset,
storage clear or physical hardware modification was performed by this session.

**Dump completeness:** 5,756 decoded record lines; indices 0–5755; sequences
**1412–7167**, consecutive and unique; **all Experiment 3**. The board reports
**Records read: 5756, invalid: 0**, followed by machine RESULT OK and a complete
END DUMP marker. Capture ended on quiet, not the 120-second cap. The observed
count is 156 more than the earlier morning INFO (5,600 / 7011), and 1,106 more
than the previous historical dump (4,650 / 6061). These are successive captures.
CRC validity is board-reported; this audit did not recompute binary CRCs from
rounded printed measurements. The text contains no firmware VERSION identity.

**Latest boot, 34:** 1,101 records, sequences **6067–7167**, all exp=3,
interval_ms=60004, epoch=0/time=UNKNOWN. Current snapshots range
**−0.671 to −0.559 mA**, voltage **13.071289–13.220507 V**, and every stored
interval charge is **−10 µAh**. FIRST_AFTER_BOOT is set for 6067 and clear for
the following 1,100 records, whose flags are zero. None of those snapshots
shows positive charging current. These observations do not prove why the
controller was off or that no brief charging occurred between snapshots.

Boot 34's first/last elapsed_ms are 78,678 / 78,441,614, a reported span of
about 21.77 hours. That is not an independently measured wall-clock duration
or uninterrupted autonomous coverage: the largest adjacent elapsed jump is
10,553,359 ms between sequences 6113 and 6114. Session/timing history must be
examined before interpreting gaps. Boot 34 also postdates the recorded boot-33
recovery smoke; no new firmware identity is inferred from the boot number.

**Hardware identity omission corrected:** the screenshot identifies the
**Ubxvamm storefront** and title containing **“5832 INA228.”** It does not
establish the actual PCB manufacturer or revision, and “5832” must not be
promoted to a revision. HARDWARE_WIRING now has a sourced hardware identity
register that distinguishes these facts. Existing Adafruit links are examples,
not verified documentation for the purchased board. The nominal R015 shunt in
the listing remains distinct from the project's 15.62 mΩ calibration.

**Session continuity:** root AGENTS.md now records operating rules and requires
maintenance of docs/CURRENT_STATE.md. INDEX links that compact current checkpoint;
the checkpoint links detailed canonical history and raw evidence. At every
meaningful handoff, record completed work, next action, ownership, validation
scope, unresolved questions and exact evidence paths. Refresh live Git state at
startup; do not treat the checkpoint's dated snapshot as a permanent fact.
The handoff does not waive the user's canonical-document read order.

## 2026-09-25: Supplied morning storage INFO — log advanced and tail intact

The user supplied a `tools/send.sh --wait LOGGER STORAGE INFO` transcript,
with terminal time approximately 08:28. The Python logger was not running.
The sender waited through at least 45 seconds of absent USB, caught
`/dev/cu.usbmodem1101`, and observed both machine ACK and RESULT OK. The
response completed on the 600 ms quiet threshold, not the capture cap.

| Reported field | Value |
| --- | ---: |
| Filesystem total | 1,441,792 bytes |
| Used / free | 413,696 / 1,028,096 bytes |
| Record size / log bytes | 72 / 403,200 bytes |
| Valid records | 5,600 |
| Oldest / newest sequence | 1412 / 7011 |
| Trailing bytes | 0 |
| Tail status | INTACT |
| Reported arithmetic capacity estimate | 14,279 more records / 9 days at 60 s |

The count and newest sequence are both **950 greater** than the last documented
4,650-record dump through sequence 6061. This establishes that the durable log
advanced while host CSV was absent for part of the elapsed time. INFO alone does
not identify every overnight record's experiment, duration, flags or readings,
or establish exact wall-clock coverage; a decoded dump is the next inspection.
The count equals 7011 − 1412 + 1, consistent with the reported sequence span.
The capacity estimate is filesystem free-space arithmetic, not a tested fill
limit. Reclamation/storage-full policy remains unimplemented.

The board was in its autonomous rendezvous and reported no host claim. The
transcript does not include VERSION, so no new firmware identity or validation
of the unuploaded storage extraction is inferred. No upload, reset, storage
clear or new hardware modification was performed in this documentation work.

## 2026-09-25: Transition report audit — overnight CSV coverage missing

The user reports leaving the setup running overnight and supplied a five-entry
`charger-transitions.py` report. Read-only inspection of the canonical repo's
CSV found **26,432 samples**, ending **2026-09-24 18:44:55.114836 −07:00**.
There are **no September 25 samples** in this file at this inspection. The
report lists state changes, not the capture end, so the report alone cannot
establish overnight coverage. **User clarification:** only the board stayed powered overnight; the Python
logger did not stay running. The missing overnight host CSV is therefore
expected, not evidence of a capture-tool failure. If autonomous mode remained
armed and writes succeeded, LittleFS records may cover time absent from host
CSV; no board query was made, so that coverage is not claimed. Experiment 3 was
not reset or cleared.

The five printed entries have different evidence:

| Reported time (UTC−07:00) | Evidence category |
| --- | --- |
| Sep 16 10:14:47 | First confirmed state in the tool's run; no confirmed preceding opposite state |
| Sep 23 09:55:29 | State after a capture gap from Sep 16; transition instant unknown |
| Sep 23 12:21:10 | State after a capture gap from 09:56:44; transition instant unknown |
| Sep 24 14:09:50 | Contiguous ON-like current edge, previously recorded |
| Sep 24 14:48:17 | Contiguous OFF-like current edge, previously recorded |

After the September 24 OFF-like edge, the inspected CSV contains **9,445
samples**, all Experiment 3, through 18:44:55. Current ranges from **−0.684 to
−0.552 mA**, voltage from **13.109375 to 13.312891 V**. The largest adjacent
capture gap in this suffix is **1,640.236589 seconds** (about 27 minutes).
These describe the observed samples, not uninterrupted coverage or a proof that
no intervening state changes occurred. Capture-gap handling remains absent
from the analysis script despite the earlier green software gate.

**Cause hypotheses, proposed by the user:** panel disconnected, insufficient
sunlight, or battery full/controller charge completion. These are useful
candidates, not an exhaustive diagnosis or identified causes of these edges.
Keep measured output state separate from an inferred reason, with explicit
UNKNOWN/ambiguous results. Time of day can inform a hypothesis; it does not
measure light at this window, establish that a cable is connected, or prove
battery state of charge. Several limiting conditions can coexist. The reason
for the 14:48 OFF-like edge remains unverified.

For later characterization, correlate continuous INA samples with timestamped
LED observations, panel/connection observations and known changes. The exact
SUNER model should be established before using its LED codes as labels.
SUNER's [BC-20W product documentation](https://sunerpower.com/products/bc-20w-poly-solar-battery-charger)
distinguishes charging, full, error and no-output indications; that is a model
example, not confirmation of the installed unit or its state.

**Additional user observations, September 25:** the panel remained connected
throughout the overnight run. It is currently cloudy and the user reports no
charging; no precise LED color/state or time was supplied. Disconnection is not
a supported explanation for this reported overnight setup. The user proposes
that a mostly topped-up battery may not trigger charging under cloudy conditions
when a lower battery would. This is a plausible interaction between available
panel power and controller restart/charge-demand behavior, not a measured rule.
The exact controller model, restart threshold/hysteresis and panel-side power
remain unverified. Time of day and cloud observations must not be promoted to
measured input-power availability or battery-full proof.

Next: inspect durable storage for overnight records before another upload,
correct gap/initial-state reporting, then close the existing storage
review/commit/hardware-smoke stage before adding firmware inference. Current
autonomous records use epoch 0 / UNKNOWN time; sequence and session-relative
fields do not by themselves give exact night/dawn wall-clock event times, and
wake snapshots cannot resolve a transition between wakes to one second.
No firmware or Python source was changed in this audit.

## 2026-09-24: Continuation audit — current tree and continuous OFF-like transition

Read-only source/git/CSV inspection, followed by documentation corrections. No
firmware or analysis code was edited, no gate was rerun, and no upload or serial
command was issued. Experiment 3 was not reset or cleared.

**Repository snapshot:** `main` at `43c3baf` (`Fix durable log recovery and
reconcile project docs`), with nothing staged. Recovery is now committed;
`eba3b5d-dirty` remains the identity of the previously supplied hardware smoke,
not a claim that clean `43c3baf` was uploaded. Storage extraction remains in the
working tree: `storage.h`/`storage.cpp` are untracked, with sketch and recovery
harness changes. Concurrent SUNER work includes `tools/charger-transitions.py`,
`pyproject.toml`/`uv.lock` changes adding pandas, and documentation changes.
The latest reported extraction result is 462 passed / 1 xfailed, 7/7 translation
units, with its full gate red on the concurrently introduced pyright error.
The CODE agent owns the Python correction and capture-gap handling; this audit
does not certify their completion or a new gate result.

**Subsequent coding-agent addendum, supplied by the user:** the pyright failure
is resolved by enumerating row positions and using `.iloc` consistently instead
of mixing index labels and positions. The agent reports byte-identical output
against its pre-fix sample capture (5 transitions, 85 lines), and a complete
`tools/check.sh` pass: exit 0, ALL OK; 462 passed / 1 xfailed; pyright,
py_compile, warning-free clean firmware compile, IntelliSense 7/7, shell syntax
and whitespace all pass. Reported flash is 1,104,313 bytes and globals 36,332
bytes. This supersedes the red-gate snapshot above. These are supplied test
results, not another gate run by this audit.

Source inspection confirms the positional fix. **Capture-gap handling remains
unimplemented:** the loop does not check timestamp spacing or experiment
boundaries before carrying candidate/confirmed state forward, and initial
state acquisition is still printed as a transition. The all-green gate does not
establish continuity-aware analysis. Storage extraction remains uncommitted and
not uploaded; its hardware smoke is still owed.

**CSV evidence, independently read from `data/samples.csv`:** all rows below
belong to Experiment 3. Times are host capture times on 2026-09-24, UTC−07:00;
they bracket observed changes at roughly one-second resolution, not exact
controller switching instants.

| Host time | Current (mA) | Voltage (V) | Observation |
| --- | ---: | ---: | --- |
| 14:09:49.468777 | -0.628 | 12.989648 | Before ON-like step |
| 14:09:50.468071 | +178.692 | 12.994336 | First positive sample |
| 14:09:51.469187 | +262.236 | 12.998047 | Positive charging current continues |
| 14:48:15.098515 | +68.748 | 13.318555 | Before OFF-like step |
| 14:48:16.097899 | +62.708 | 13.318555 | Last positive sample in inspected capture |
| 14:48:17.099301 | -0.624 | 13.312891 | First near-zero/reverse sample |
| 14:48:18.099522 | -0.644 | 13.311719 | Near-zero/reverse current continues |
| 14:48:19.103015 | -0.636 | 13.310547 | Near-zero/reverse current continues |

The OFF-like step spans **1.001402 seconds** between adjacent captured samples,
so it is not a before/after comparison across a capture gap. The inspected
suffix contains **1,728 samples through 15:17:04.967150**, ranging from
**−0.684 to −0.564 mA**. This supersedes the kickoff's latest-known statement
that charging continued and no continuous OFF transition had been captured.
It establishes a sustained current change, not its cause or the controller's
internal cutoff rule. A matching LED observation has not yet been supplied.

The user additionally reports approximately **3.5 minutes** from connecting the
series resistor load to charging resuming. This refines the earlier "few
minutes" report; the load-attachment timestamp and resistor temperature were
not independently measured here. The resistor heat observation remains
qualitative. Production charger-output detection is intended to operate directly
on INA readings in firmware; CSV analysis is characterization only. Thresholds,
persistence and sleep/sampling integration remain undecided.

**Documentation drift corrected:** PROJECT's next-stage paragraph still placed
LittleFS in the sketch; BACKLOG's recovery entry still put scan/truncation there;
STORAGE_SYNC_DESIGN called 429/1 the latest gate and its lifecycle diagram still
put DIAG_ALRT after CHARGE/ENERGY. The diagram now matches the implemented order.
Older dated lab entries retain their at-the-time status and are superseded by
D-058/current module inventory for storage extraction. No new hardware validation
is claimed.

## 2026-09-24: SUNER addendum — first contiguous charger-ON transition

**OBSERVED, as supplied by the user:** this adds timing and sample-analysis
results to the earlier resistor-load experiment below. The analysis run and
physical observations were supplied; this documentation stint did not rerun the
tool, perform a new hardware test, upload or access serial. Source inspection
confirms the analysis tool's current behavior and its capture-gap limitation.

### Load timing and heat — approximately 3.5 minutes

The two **6 Ω / 50 W aluminum resistors in series** formed a nominal **12 Ω**
load. At 12.9–13.2 V, the calculated load remains **1.075–1.100 A**, about
**14 W total / 7 W each**, as detailed in the earlier entry; these are nominal
calculations, not measured resistor-branch current.

After only **about 3.5 minutes**, the resistors were **SCALDING / VERY HOT in
free air**, and the battery had been pulled down enough for the SUNER to resume
its **blinking red charging indication**. The load was disconnected early for
both reasons: the desired transition had occurred and the resistors were
extremely hot. The original plan was up to 15 minutes, not the observed duration.

No resistor case temperature was measured. A 50 W aluminum-resistor rating does
not mean it stays cool at approximately 7 W in free air; chassis/heatsink
assumptions matter. No precise temperature or free-air power limit is inferred.

### First directly observed NOT_CHARGING → CHARGING transition

The logger was running continuously before the load was connected. The supplied
`samples.csv` analysis identified this contiguous transition on **2026-09-24**:

| Timestamp as supplied | Voltage (V) | Current (mA) | Power (mW) | Analysis state |
| --- | ---: | ---: | ---: | --- |
| 2026-09-24 14:09:47.468 | 12.998438 | −0.608 | 7.9104 | NOT_CHARGING |
| 2026-09-24 14:09:48.468 | 12.994141 | −0.624 | 8.1024 | NOT_CHARGING |
| 2026-09-24 14:09:49.469 | 12.989648 | −0.628 | 8.1792 | NOT_CHARGING |
| 2026-09-24 14:09:50.468 | 12.994336 | +178.692 | 2322.8032 | CHARGING |
| 2026-09-24 14:09:51.469 | 12.998047 | +262.236 | 3408.5504 | CHARGING |
| 2026-09-24 14:09:52.467 | 12.996289 | +261.340 | 3396.4544 | CHARGING |

The excerpt supplies no UTC offset; these timestamps are reproduced without
conversion. The first positive charging sample is **14:09:50.468**, following
−0.628 mA at 14:09:49.469, then +262.236 mA one sample later. The change is
observed between adjacent approximately one-second samples; the exact physical
switching instant within that interval is not established. Analysis-state labels
are not new firmware telemetry fields.

The SUNER **blinking red LED was observed during the charging regime**, strongly
correlating positive INA current with controller output. No precisely timestamped
LED edge was supplied. This is direct evidence for an INA-based **CHARGER_OUTPUT**
state (`CHARGING`, `NOT_CHARGING`, `UNKNOWN / TRANSITION`), not proof of
**SOLAR_AVAILABLE**. Near-zero output can still mean full-battery/controller
cutoff, nighttime, panel disconnection or controller fault.

**At the supplied observation cutoff the SUNER was STILL CHARGING.** Today's
continuous CHARGING → NOT_CHARGING transition had not yet been captured. Capture
that OFF edge and correlate it with the LED before claiming both directions are
characterized. This is a dated observation, not a live board-status assertion.

**Later evidence, preserved separately:** the concurrent continuation audit above
records an OFF-like current step at **14:48:17.099301 −07:00**. A read-only CSV
spot-check in this stint confirms +62.708 mA at 14:48:16.097899 followed by
−0.624 mA, with 1,728 near-zero/reverse samples through 15:17:04.967150
(−0.684 to −0.564 mA). This supersedes the brief's missing-current-edge status,
not its missing LED correlation. The cause/controller state is still unverified
by a matching LED observation; both directions are not fully characterized.

### Characterization tool, capture gaps and future firmware

[tools/charger-transitions.py](../tools/charger-transitions.py) now exists and
the supplied run found the ON transition above. It analyzes `data/samples.csv`
with provisional current/voltage classification and consecutive-row persistence,
then prints state changes and surrounding samples. These are **characterization
tools**, not the intended production dependency. Future firmware would infer
charger output directly from INA228 readings on the ESP32, after characterizing
ON/OFF shapes, thresholds, hysteresis, persistence/debounce, noise and ambiguous
regions. No firmware thresholds or final algorithm have been decided.

**OPEN tool issue: consecutive rows need not be contiguous in time.** Source
inspection shows no elapsed-time gap check or separate initial-state category.
Historical capture included CHARGING around **2026-09-23 09:56**, then no samples
until roughly **12:21**, when the first later samples were NOT_CHARGING. A state
change occurred somewhere in the gap; its exact time is **UNKNOWN**. Neither
the first post-gap sample nor the first state in a file is a precisely observed
transition. Add distinct `INITIAL STATE`, `STATE AFTER CAPTURE GAP` and
`OBSERVED CONTIGUOUS TRANSITION` reports, requiring sufficiently contiguous
evidence on both sides of an observed edge. Gap and persistence criteria remain
to be characterized; no numeric maximum gap is chosen here.

While the ESP32 is **awake**, successive INA readings can support high-resolution
transition detection. In normal autonomous **deep sleep**, the INA keeps operating
but the ESP is not continuously evaluating samples. Without a future hardware
alert/wake mechanism, state can only be assessed at normal wake/sample points;
the wake snapshot does not establish second-level timing of a change during
sleep. INA228 alert-pin wake is a possible later investigation, **not implemented
or decided**. The INA-OFF power-test variant is a separate mode that deliberately
stops INA measurement.

Placement remains **INFERRED, NOT YET PHYSICALLY TRACED**: the nominal ~1.07 A
load did not appear as an approximately −1 A INA shift, while positive controller
charge current was observed. This suggests controller charging-path current,
not total battery net current; no terminal/polarity map is established. Wiring
is unchanged. See [HARDWARE_WIRING.md](HARDWARE_WIRING.md) and the
[characterization backlog](BACKLOG.md#characterize-suner-charger-state-inference-from-samplescsv).

## 2026-09-24: SUNER controller restart under a temporary resistor load

**OBSERVED, as reported by the user:** this records an already completed bench
experiment and the supplied telemetry excerpts. No new hardware test or CSV
analysis was performed by this documentation stint. Exact event timestamps,
restart/cutoff thresholds and a complete OFF transition are not established by
these excerpts. Existing firmware-validation history remains separate.

### Purpose and temporary load

The battery had been near the SUNER controller's full/cutoff-like state:
approximately **13.1 V, −0.6 mA**, and **8 mW magnitude** at the INA228. A
temporary resistive load was applied to lower battery voltage and observe the
controller restarting charge. The logger was already running before the load
was applied, so continuous telemetry captured the transition.

Two newly arrived **6 Ω, 50 W aluminum-housed power resistors** were connected
**in series**, giving a nominal **12 Ω** load. Exact attachment points and
high-current routing were not supplied as a physical trace.

**CALCULATED, not measured load current or temperature:** for 12.9–13.2 V across
the nominal 12 Ω series pair, `I = V/R` and `P = V²/R` give:

| Quantity | At 12.9 V | At 13.2 V |
| --- | ---: | ---: |
| Load current | 1.075 A | 1.100 A |
| Total dissipation | 13.8675 W | 14.5200 W |
| Dissipation per equal 6 Ω resistor | 6.93375 W | 7.2600 W |

Thus the expected load was roughly **1.07–1.10 A**, **14 W total**, or **7 W per
resistor**. These estimates use nominal resistance; they are not INA readings
of the resistor branch.

**THERMAL OBSERVATION:** the resistors became **VERY HOT in free air**. No case
temperature was measured. The practical lesson is that a 50 W headline rating
does not mean a resistor stays cool at lower power: aluminum power resistors
can become very hot without substantial heat sinking/chassis mounting. This
experiment does not establish the particular parts' permitted free-air power.

The SUNER returned to its **blinking red charging indication within a few
minutes**. The resistors were then disconnected; the intended transition had
occurred, so there was no need to continue the planned 15-minute load.

### Charging telemetry and behavior after removing the load

Blinking red was visually observed as the controller's charging indication,
correlated with approximately **12.883 V, +154–160 mA and 1.99–2.05 W**.
A representative interval excerpt was:

```text
CSV_DATA,3,273,60.000,12.884375,154.916000,1995.993600,...
```

The ellipsis denotes omitted fields; this is not a complete CSV row.

After load removal the controller remained blinking red. Voltage rose from
roughly **13.2154 V to 13.2205 V**, while current remained **+163–167 mA** and
power **2.16–2.21 W**. Representative supplied samples:

| Voltage (V) | Current (mA) | Power (mW) |
| ---: | ---: | ---: |
| 13.215430 | 164.432 | 2173.0304 |
| 13.216211 | 163.880 | 2165.8752 |
| 13.218945 | 166.128 | 2196.0192 |
| 13.220117 | 166.932 | 2206.8352 |
| 13.220508 | 166.316 | 2198.7840 |

Charging persisted while voltage rose after the temporary load was removed,
consistent with the controller remaining latched in active charging. This does
not determine its internal control logic or numeric restart/cutoff hysteresis.
The live chart showed the major load/controller transition and a later narrow
dip associated in time with physically checking the LED. **The narrow dip's
cause is unknown**; temporal association is not a causal explanation.

### OBSERVED INFERENCE FROM TELEMETRY — current-path interpretation

The nominal resistor load was approximately 1.07 A. If the INA228 measured
total net battery current including that load, connecting it would be expected
to produce an approximately −1 A shift, other contributions being comparable.
Instead, reported current during charging was approximately **+150–170 mA**.

This strongly indicates that the present INA228 placement measures the
**SUNER/controller charging path rather than total net battery current including
the temporary resistor branch**. It is an **observed inference from telemetry**,
not a fully traced wiring fact. Exact controller/shunt/battery polarity and
routing remain OPEN pending physical setup/photo/wiring trace. See
[HARDWARE_WIRING.md](HARDWARE_WIRING.md#current--high-current-measurement-path).

### Charger-output inference — promising, not implemented or fully characterized

The observed full/cutoff-like regime near **−0.6 mA** and active charging near
**+155–167 mA** differ by more than two orders of magnitude in current magnitude.
The simultaneous blinking-red indication strongly correlates with the positive
charging regime. A future classifier could describe **CHARGER_OUTPUT** as
`CHARGING`, `NOT_CHARGING`, or `UNKNOWN / TRANSITION`.

Do not call that result **SOLAR_AVAILABLE**. Near-zero controller-output current
could mean a full battery/controller cutoff in bright sun, nighttime, a
disconnected panel, or controller fault. This current alone cannot distinguish
those causes. The narrower question is: **is the SUNER currently delivering
charging current?** It is not a battery state-of-charge measurement either.

**No production thresholds are decided.** Illustrative future logic could use
hysteresis and persistence: sustained current well above zero suggests charging;
near-zero/slight reverse current suggests not charging; intermediate/noisy
values could mean transition or retention of the prior state. This is a proposal,
not an implemented detector or a chosen threshold policy.

Next, analyze `data/samples.csv` for sustained current step changes, using a
rolling median or similar smoothing and a configurable delta threshold. Report
timestamps and before/after current, voltage and power, with surrounding samples
for inspection. The OFF transition is **not yet fully captured/correlated**:
compare the LED transition to not-charging with the matching INA-current step
before declaring an INA-only classifier fully characterized. The work is tracked
in [BACKLOG.md](BACKLOG.md#characterize-suner-charger-state-inference-from-samplescsv).
No formal architecture decision or firmware/UI feature was added.

## 2026-09-24: Hardware validation addendum — clean record format and working-tree recovery

**OBSERVED, as supplied by the user:** the results below record two completed
board runs. This documentation reconciliation inspected current source/tests;
it did not build, upload, access serial, rerun `tools/check.sh`, or perform a new
hardware test. Older entries retain their status at the time of each stint;
this addendum supersedes their pending-hardware status, within the limits below.

### Clean record-format image — eba3b5d

`tools/upload.sh` successfully built and uploaded clean revision **`eba3b5d`**.
Firmware reported version **`0.3.0-dev`**, revision **`eba3b5d`**, and build ID
**`solar-logger-protocol-ack-v3`**. Experiment 3 survived. Historical records
decoded and new autonomous records appended with valid CRCs. AutoRecord stayed
**72 bytes, version 1**, with no format change or migration.

The earlier extraction gate was **387 passed, 1 xfailed**. Its deployed golden
Experiment 3 fixture (`seq=4445`, CRC `0xFB25DA73`) establishes CRC coverage of
bytes 0–67, CRC at offset 68, and the `esp_rom_crc32_le(0, ...)` / IEEE-zlib
convention. That host fixture and the post-upload records are separate evidence.

Storage progressed from **4,528 valid records / newest sequence 5939** to
**4,530 / 5941** after two autonomous wakes. The subsequent dump included:

| Sequence | Boot | Experiment | Interval (ms) | Flags | CRC |
| ---: | ---: | ---: | ---: | --- | --- |
| 5940 | 32 | 3 | 60004 | `0x4[FIRST_AFTER_BOOT]` | `0x57D6B79C` |
| 5941 | 32 | 3 | 60004 | `0x0[none]` | `0x8E93D0A2` |
| 5942 | 32 | 3 | 60004 | `0x0[none]` | `0x1E771662` |

Final dump: **Records read: 4531; invalid: 0**. Trailing bytes were **0** and
tail status **INTACT**. These successive snapshots are not conflicting totals:
another record was appended before the final dump.

**Record format is IMPLEMENTED, SOFTWARE-TESTED and HARDWARE-VALIDATED.** The
smoke covers old-record compatibility, ESP32 CRC/validation, new record creation
and append, sequence progression, FIRST_AFTER_BOOT set then cleared, and
Experiment 3/log integrity. Telemetry also ran in this smoke-validated image;
the supplied evidence does not claim an exhaustive comparison of all CSV forms.

### Storage-recovery working-tree image — eba3b5d-dirty

The subsequent upload contained the uncommitted recovery fix and reported
**`eba3b5d-dirty`**. This is a **storage-recovery working-tree build based on
`eba3b5d`**, not a clean commit containing that fix. Version and build ID remain
`0.3.0-dev` / `solar-logger-protocol-ack-v3`.

| Observation | Record size | Valid records | Oldest sequence | Newest sequence | Trailing bytes | Tail status |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Immediately after upload | 72 bytes | 4644 | 1412 | 6055 | 0 | INTACT |
| After additional autonomous wakes | 72 bytes | 4649 | — | 6060 | 0 | INTACT |

Final dump included **6057, 6058, 6059, 6060 and 6061**, all with **boot=33,
exp=3, interval_ms=60004**, valid CRCs and normal flags. Its summary was
**Records read: 4650; invalid: 0**. Existing records continued decoding, new
records continued appending, Experiment 3 remained intact, and the tail stayed
INTACT. No corruption was injected into the live log.

**Recovery is corrected, software-tested and NORMAL-PATH HARDWARE-SMOKE
VALIDATED.** The smoke exercised a healthy log only. Torn-tail repair,
complete-invalid-tail repair, mid-file corruption refusal and read-error
refusal remain **host/synthetic tested, not exercised on physical hardware**.
Any later destructive/recovery hardware test requires a disposable/test log;
do not inject corruption into Experiment 3.

### Latest reported software gate and remaining boundaries

The latest coding-agent gate after recovery, **not rerun by this doc stint**:

| Check | Reported result |
| --- | --- |
| pytest | **429 passed, 1 xfailed** |
| pyright | clean |
| py_compile | pass |
| Arduino clean compile, `--warnings all` | pass |
| IntelliSense | **6/6 project translation units** |
| Shell syntax | pass |
| `git diff --check` | pass |

The six units are `connection.cpp`, `ina228.cpp`, `nvs_persistence.cpp`,
`record_format.cpp`, `telemetry.cpp`, and `solar-logger.ino`. Discovery remains
automatic. The one xfail is the existing **OPEN short-cadence autonomous overrun
defect**, not a regression or a fix.

Recovery added **42 tests**, without changing pre-existing tests, executing the
firmware scan/recover/truncate/info functions against a file-backed LittleFS
harness and linking real `record_format.cpp`. The coding agent reported mutation
failures of **12** tests for restoring the first-invalid `break`, **8** for
removing destructive-repair refusal, and **3** for removing read-error handling.
Reported flash: **1,102,453 → 1,103,995 bytes (+1,542)**; globals **36,332 bytes,
unchanged**. These are that gate's build figures, not new measurements here.

DIAG_ALRT ordering is fixed, source/regression-tested and hardware-smoke
validated: validate INA → DIAG_ALRT → CHARGE → ENERGY. **A real accumulator
overflow has not been induced; INA_ACCUM_OF has not been observed SET from one.**
The hardware smoke does not validate that overflow branch. Pre-fix records are
not retroactively qualified; tethered overflow qualification and MATHOF interval
semantics remain open.

There is still no operator mid-file repair/quarantine command, no resync after
non-record-sized mid-file insertion (such as `[10][8 junk bytes][11]`), and no
physical read-error observation. CURRENT/CHARGE remain signed; POWER/ENERGY are
unsigned magnitudes, so running energy is not a signed/net energy balance.

The previous correctness blocker before LittleFS extraction is resolved within
the stated limits. **Next structural candidate: extract LittleFS/durable-storage
mechanics**, preserving D-057 and its remaining edge cases. It has not been
extracted. Sync/ACK/reclamation, storage-full policy, time sync, BLE/iOS/server,
manual SYNC circuitry and IBS integration remain unimplemented; current wiring,
future regulator design and open BMW trunk topology are unchanged.

## 2026-09-24: One corrupt record no longer discards the valid records after it - NOT hardware validated

A correctness fix to durable-log boot recovery. Nothing was uploaded, no serial
port was opened, and no board was touched. The live Experiment 3 log was never
read, written or used as a test fixture: every damaged log in this work is
synthetic and lives in a pytest temporary directory.

**This is not a hardware result.** The image was compiled and the host tests were
run. The DIAG_ALRT reorder from 2026-09-23 and the record-format extraction from
earlier on 2026-09-24 still owe their own hardware checks, and this stint does
not supply them or establish a newer board state.

### The defect, measured rather than inferred

`autoStorageScan()` walked the log forward and stopped at the first record that
failed validation. `scan.trailingBytes` then became every byte from there to end
of file, and `autoStorageRecover()` handed that to `autoStorageTruncateToValid()`
at every cold boot and every RTC-loss wake.

For a torn tail that is correct, which is why it never showed on hardware. For a
bit flip in the middle of the log it deletes every good record after the damage.

A host harness was built that compiles the firmware's own
`autoStorageMount/Scan/TruncateToValid/Recover` and `printStorageInfo` out of the
sketch and runs them against a LittleFS made of real files, linking the real
`record_format.cpp` so `autoRecordValid()` is the actual validator. Run against
the **pre-fix** code:

| Log laid down | Bytes in | Bytes out | Sequences surviving |
| --- | ---: | ---: | --- |
| `[seq 10][seq 11][seq 12]` | 216 | 216 | 10, 11, 12 |
| `[seq 10][seq 11][40 loose bytes]` | 184 | 144 | 10, 11 |
| `[seq 10][seq 11][bad seq 12]` | 216 | 144 | 10, 11 |
| **`[seq 10][bad seq 11][seq 12][seq 13]`** | **288** | **72** | **10** |
| **`[seq 10][bad seq 11][seq 12][bad seq 13]`** | **288** | **72** | **10** |

The first three rows are the intended behavior. The last two are the defect:
sequences 12 and 13 were complete, CRC-correct records, and boot recovery deleted
them. `LOGGER STORAGE INFO` on the same log reported **1** valid record when
three existed, and `Tail status: INVALID RECORD DETECTED`, so an operator
deciding what to do was told less good data was at stake than actually was.

The loss was counted and printed. It was still automatic, irreversible, and it
happened before anyone read the message.

### What changed

Recovery now distinguishes four damage cases instead of collapsing them, and
`printStorageInfo()` reports them separately. The rule and its reasoning are
D-057; Section 11 of STORAGE_SYNC_DESIGN.md has been reconciled with the code.

- `autoStorageScan()` scans the whole file, counting every invalid record, the
  first one's offset, and how many valid records are stranded past it.
- `trailingBytes` now means what an automatic repair would discard — the unbroken
  run of invalid records at the end, plus a partial remainder — and `keepBytes`
  what it would keep.
- A valid record after an invalid one means nothing is discarded. The refusal is
  enforced inside `autoStorageTruncateToValid()` as well as in its caller.
- A short read while scanning sets `readError` and repairs nothing. An I/O fault
  is not a torn tail, and it is the one case where what is damaged is not even
  known. It is staged in the tests by telling the firmware the log is 72 bytes
  longer than it is, which is the only way to make a real file read short.

Re-run after the fix, the same five logs:

| Log laid down | Bytes in | Bytes out | Sequences surviving |
| --- | ---: | ---: | --- |
| `[seq 10][seq 11][seq 12]` | 216 | 216 | 10, 11, 12 |
| `[seq 10][seq 11][40 loose bytes]` | 184 | 144 | 10, 11 |
| `[seq 10][seq 11][bad seq 12]` | 216 | 144 | 10, 11 |
| `[seq 10][bad seq 11][seq 12][seq 13]` | 288 | **288** | 10, 11, 12, 13 |
| `[seq 10][bad seq 11][seq 12][bad seq 13]` | 288 | **288** | 10, 11, 12, 13 |

The first three are byte-identical to before. The last two discard nothing and
say so.

### Sequence authority

`logIntact` already required `invalidRecords == 0`, so a preserved mid-file
corruption keeps the NVS reservation floor in force, unchanged from D-023. The
log's own candidate is now one past the last valid record in the whole file: for
the fourth row above it is 14, where the pre-fix code returned 11 while deleting
the records that had used 12 and 13.

### Evidence and its limits

`tests/test_characterization_storage_recovery.py`, 42 tests, all executing the
firmware's own compiled functions. The gate is green:
**429 passed, 1 xfailed** (387 + 1 before this work; the xfail is the unrelated
short-cadence autonomous overrun defect). Flash went from 1,102,453 to 1,103,995
bytes, **+1,542**, both 84% of `app0`. Global variables are unchanged at 36,332
bytes: `AutoLogScan` is a stack local, and the five fields added to it cost no
static memory.

The tests were confirmed to fail against the behavior they replace. Restoring the
scan's `break` at the first invalid record turns **12** of them red; keeping the
full scan but removing the refusal and the trailing-run arithmetic turns **8**
red. They are load-bearing in both directions.

**What this does not show.** No board has run this code. The only hardware
evidence about recovery remains the 2026-09-23 `d017f7d` snapshot of an
*undamaged* log, and no corrupt record has ever been observed on this hardware.
When hardware validation is requested it should confirm normal valid-log
behavior only — an intact Experiment 3 log scanning, decoding and appending as
before — unless a separate disposable log or isolated storage is created first.
Corruption must not be injected into the live log.

### Left unsolved, deliberately

Damage that is not a whole number of records throws every later record off the
72-byte grid, so nothing after it validates, no valid suffix is detectable, and
it is discarded as a torn tail. `[seq 10][8 junk bytes][seq 11]` goes in at 152
bytes and comes out at 72, before and after this fix alike. Recovering it needs a
resynchronization policy this project does not have; it is recorded in BACKLOG.md
and pinned as-is by a test that says so.

There is also no operator command to repair mid-file damage once it is reported.
Adding one changes the command protocol and its build ID, so it was kept out of a
correctness fix.

## 2026-09-24: Record format extracted into a module - NOT hardware validated

Structural extraction only. The deployed 72-byte durable record and its CRC
helpers moved out of `solar-logger.ino` into `record_format.h` and
`record_format.cpp`. No behavior change was intended and none was found. Nothing
was uploaded, no serial port was opened, and no board was touched.

**This is not a hardware result.** The image was compiled and the host tests
were run. The DIAG_ALRT reorder from 2026-09-23 still owes its own hardware check
of the overflow path, and this stint does not supply it or establish a newer
board state.

### What moved

Into `record_format.h`: `AUTO_RECORD_MAGIC`, `AUTO_RECORD_VERSION`, the eight
record flag bit values and the time-quality mask, `AUTO_EXPERIMENT_UNKNOWN`, the
two snapshot-unread sentinels, the packed `AutoRecord` struct, its
`static_assert(sizeof(AutoRecord) == 72)` and `AUTO_RECORD_SIZE`.

Into `record_format.cpp`: `autoRecordCrc()`, `autoRecordValid()` and
`autoRecordSnapshotUnread()`, with `#include "esp_rom_crc.h"`, which the sketch
no longer needs because `autoRecordCrc()` was its only user.

### What deliberately did not move

`printAutoRecord()` and `autoTimeQualityName()`. They interleave flag
interpretation with `Serial` formatting and comma bookkeeping for the
`LOGGER STORAGE DUMP` transcript, which is an output format rather than the
record format; moving them would have made this a formatting rewrite. Both are
token-identical to `HEAD` and still in the sketch.

Also unmoved, and verified as such: LittleFS mount, append, scan, truncation and
tail recovery; the twelve `RTC_DATA_ATTR` declarations; `reserveSequenceBlock()`
and the NVS sequence high-water mark; `AUTO_SEQ_BLOCK`; `AUTO_LOG_PATH`; the
autonomous scheduler and wake cycle; host session logic. A test asserts the
module names no `LittleFS`, `Preferences`, `Serial`, `RTC_DATA_ATTR`, `millis`,
`esp_sleep`, `AUTO_SEQ_BLOCK` or `AUTO_LOG_PATH`, and contains no `flags |=`.

### The move audit (D-047)

Run as a scratch script over the tokenizer in `tests/firmware_source.py`, the
same way earlier stages did. Three claims, each checked:

- **Reconstruction.** Deleting 132 lines from `HEAD`'s sketch and inserting one
  `#include "record_format.h"` reproduces the new sketch **token for token**:
  18,371 tokens to 18,080, and 7,088 lines to 6,963.
- **Nothing lost.** All 292 tokens that left the sketch are present in the two
  new files, counted as a multiset.
- **Nothing rewritten.** The three moved function definitions, the struct's
  members and its packed head are token-identical to `HEAD`. So are the two
  functions that stayed.

The moved regions were also diffed as raw bytes against the originals, and are
byte-identical including indentation and comments.

**One comment was edited, stated here because the token audit strips comments and
cannot see it.** The note beside `AutoLogScan` said it was declared "next to the
record", which the move made false. The substantive half of that comment — that
the Arduino preprocessor emits generated prototypes ahead of the first function
definition, so a type named in a prototype must exist by then — is unchanged.

### What was strengthened, and why that is not scope creep

`sizeof(AutoRecord) == 72` moved unchanged, but on its own it cannot see two
same-width fields swap places, a signed field turn unsigned, or a widened field
paid for by a narrowed neighbor. All three keep the size at 72, and all three
make Experiment 3's log unreadable. A file move is exactly when those mistakes
happen, so the header now asserts every field's `offsetof`, width and signedness,
and that `crc32` is the last field so the sealed range really is bytes 0–67.

Both additions were verified by construction rather than assumed:

| Injected fault | `sizeof == 72` | New assertions |
| --- | --- | --- |
| `int32_t avg_current_uA` to `uint32_t` | passes | 3 failures |
| `boot_id` swapped with `session_elapsed_ms` | passes | 6 failures |

### Tests

54 added, all in `tests/test_characterization_record.py`. **No existing test was
changed.** `firmware_source.py` reads every sketch file as one, so the 86 record
tests written before the move kept passing across it without an edit, which is
what that design is for.

The new tests fall in three groups:

- **The boundary.** The helpers are defined in `record_format.cpp` and nowhere
  else; the struct and every frozen constant are in `record_format.h` and not in
  the sketch; each frozen constant is defined **exactly once** across all sketch
  files; the printing helpers stayed in the sketch; the module holds no policy,
  state or I/O; every field's compile-time offset and signedness assertion is
  present in source.
- **The module's own code, compiled and run.** `record_format.cpp` itself — not
  an extracted copy — is compiled on the host and run against the real
  Experiment 3 record. It reproduces `0xFB25DA73`, accepts that record, rejects a
  wrong magic and a wrong version **even after the CRC is recomputed over the
  corrupted bytes** (so the checks are proved individually rather than hidden
  behind a failing checksum), rejects all 72 single-byte corruptions, and
  recognizes the snapshot-unread sentinels.
- **`printAutoRecord()`, which had no test at all before.** It now reproduces the
  verbatim `LOGGER STORAGE DUMP` tail the board printed for `seq=4445`, decodes
  all six flag names with their comma bookkeeping, names each of the four
  time-quality values including the unassigned `RESERVED`, and names an unread
  snapshot instead of printing 4294.967295 V.

**One substitution, stated rather than buried.** `esp_rom_crc32_le()` lives in
the ESP32 mask ROM and cannot be linked on the host, so the harness supplies its
own. On its own that would prove nothing. It is asserted equal to the CRC the
**board** stored for `seq=4445`, so the chain is stub = zlib = what the ROM did on
hardware. What the substitution does not weaken: the struct's layout on a real
compiler, the 68-byte covered range, the CRC offset, the magic and version
checks, and every byte of the transcript line.

### Gate

`tools/check.sh`, before and after.

| | Before | After |
| --- | --- | --- |
| pytest | 333 passed, 1 xfailed | **387 passed, 1 xfailed** |
| pyright | 0 errors | 0 errors |
| py_compile | pass | pass |
| ESP32 compile, `--clean --warnings all` | pass, no warnings | pass, no warnings |
| IntelliSense translation units | 5 | **6** |
| shell syntax | pass | pass |
| whitespace | pass | pass |
| | `ALL OK` | `ALL OK` |

The xfail is the same one: the short-cadence autonomous overrun defect. It was
not touched, suppressed or converted.

`record_format.cpp` needed **no** change to `tools/` or `.vscode/`. The gate
discovered it and verified its editor entry by compiling it with exactly those
flags (D-048), which is the third module in a row for which that held.

### Flash and globals, with the 2 bytes accounted for

| | Before | After |
| --- | ---: | ---: |
| Program storage | 1,102,455 | 1,102,453 |
| Global variables | 36,332 | 36,332 |

Both images were rebuilt from clean into separate build paths, and the before
figure reproduced `HEAD`'s exactly. A per-symbol comparison of the two ELFs finds
**one** symbol changed size: `autoRecordValid()`, 96 bytes to 94.

Its instruction sequence is identical. The single difference is the call to
`autoRecordCrc()`, an 84-byte backward jump in both images, encoded as a 4-byte
`jal` in the old layout and a 2-byte compressed `c.jal` in the new one. The
distance did not change, so this is GNU ld's RISC-V relaxation reaching a
different fixed point after the new section placement, not a code change. The
other two call sites of `autoRecordCrc()` are far away and stayed 4-byte `jal` in
both.

Globals unchanged is the measurement that matters for the boundary: the module
owns no state.

### Recommended hardware smoke test

Unchanged behavior means the check is that nothing regressed, and the sharpest
available evidence is that the existing log stays readable:

1. `tools/upload.sh`
2. During the 15-second cold-boot maintenance window: `LOGGER STORAGE STATUS`.
   Expect Experiment 3 preserved, the same record count as before the upload plus
   any new autonomous records, `0` invalid records, trailing bytes `0` and tail
   status `INTACT`.
3. `LOGGER STORAGE DUMP`. Every pre-existing record must still validate and print
   as before; a layout or CRC regression would show as invalid records rather
   than as a crash.
4. Let one autonomous wake complete and confirm a new record appends and
   validates, with `seq` continuing to advance.

A real INA accumulator overflow is still not induced by this, so
`INA_ACCUM_OF` remains unobserved on hardware.

## 2026-09-24: Current bench wiring documented — no new hardware measurement

[HARDWARE_WIRING.md](HARDWARE_WIRING.md) now owns the user-reported current
logic wiring and Mermaid diagram. USB powers the XIAO; its 3V3 powers INA228
logic. The loose VUSB jumper has no connected far end and should be removed.
Source inspection confirms D4/SDA, D5/SCL and 400 kHz, not physical routing.

No existing canonical diagram was found. Exact SUNER/shunt/battery polarity
routing remains OPEN pending physical setup/photo capture, so no complete
measurement-path schematic or SVG was added. Earlier AA/VUSB power tests
retain their dated conditions; PROJECT now labels their reproduction
procedure explicitly as historical. Future regulator design and BMW trunk
topology remain unresolved. This stint changed documentation only, with no
build, upload, serial access or new hardware validation.

## 2026-09-23: Installed architecture decisions and modularization-plan correction — documentation only

Recorded from the user's architecture brief, not from a hardware experiment.
No firmware, tests or tooling were edited by this documentation stint; no build,
upload or serial operation was performed, and no commit was made. Existing
coding-agent changes and results remain separate from these decisions.

D-052 settles V1 electrical attachment at the designated under-hood charging/
jump points with the logger in the glove box/cabin, with neither an IBS/LIN tap
nor a separate ignition wire for vehicle-on detection. The later IBS phase
expects a trunk enclosure, but trunk electrical topology and F22 signal access
remain open research; no authoritative BMW answer was found in the repository.

D-053 selects BLE/iPhone as the installed path: phone durable save before ACK,
phone-supplied time and phone-to-server upload. The ESP has no home-server or
home-Wi-Fi dependency. D-054 retains manual physical SYNC permanently; exact
vehicle-on wake and a possible vehicle-active mode remain future design work.
None of these installed features is claimed built or measured.

D-055 replaces the broad proposed `auto_state` grouping with separate record/
CRC, LittleFS mechanics, sequence-authority, retained-state and scheduling/
session concepts. Exact future filenames/APIs remain undecided. The old graph
analysis is labeled historical rather than presented as proof of this plan.

The coding-agent entry below remains the authority for its source tests and
gate result. The DIAG_ALRT fix was source-tested, gate green, not uploaded and
not hardware validated at that report. Record format/version did not change:
version 1, 72 bytes, CRC covers 0–67 and is stored at 68, with the real
Experiment 3 record matching IEEE/zlib CRC-32. That deployed-record evidence is
not hardware validation of the reorder. `record_format` remains unextracted.
The tethered overflow gap, MATHOF interval limits, signed-charge versus unsigned
magnitude-energy/net-energy question, corrupt-tail behavior, overrun xfail and
unimplemented sync/ACK/reclamation/full-storage policy remain open.

Documentation drift corrected here: the obsolete telemetry-in-progress
paragraph, USB-first installed clock priority, optional-only manual access,
and the combined `auto_state` plan. BMW wiring questions were added without
answering them. Historical measurements and the coding-agent report below are
preserved.

## 2026-09-23: DIAG_ALRT read order confirmed WRONG and FIXED; golden Experiment 3 record proves the 72-byte layout - record-format extraction NOT started

**No hardware was touched.** Nothing was uploaded or committed, Experiment 3 was
not reset, storage was not cleared, and no serial port was opened. **No overflow
was induced on hardware, so the fixed path has never been seen to set the flag.**

One runtime behavior change was made, after it was authorized explicitly: the
`DIAG_ALRT` read order fix described below. The `record_format` extraction was
NOT started.

### What this stint was for, and why it stopped

The stint was to extract the 72-byte durable record format and its CRC into a
`record_format` module, after first resolving the "overflow-read ordering"
question raised by the 2026-09-23 documentation audit. Its contract said that a
real correctness defect stops the extraction and gets reported on its own,
because a behavior fix must not ride inside a file move.

The question turned out to be a real defect, so the extraction stopped and was
reported. **The extraction was not started** — no `record_format.h` / `.cpp`
exists. The ordering fix itself was then authorized explicitly, as its own change,
and applied; the file move was not.

### The defect

`runAutonomousWakeCycle()` read the accumulators and then read the flag that
qualifies them. The read destroys the flag.

Order in the pre-fix source, at commit `79519df`, by in-function offset rather
than line number so the reference survives the fix that follows:

| Order | What happened |
| ---: | --- |
| 1 | `readAccumulatedCharge_mAh()` → reads CHARGE (0Ah), **clears CHARGEOF** |
| 2 | `readAccumulatedEnergy_mWh()` → reads ENERGY (09h), **clears ENERGYOF** |
| 3 | `readRegister16(REG_DIAG_ALRT, diag)` → reads 0Bh, **too late, always 0** |
| 4 | sets `AUTO_FLAG_INA_ACCUM_OF`, which could never be reached |

Current line numbers after the fix: `DIAG_ALRT` at
[solar-logger.ino:3977](../Arduino/solar-logger/solar-logger.ino#L3977), the flag
at [:3989](../Arduino/solar-logger/solar-logger.ino#L3989), CHARGE and ENERGY at
[:4006](../Arduino/solar-logger/solar-logger.ino#L4006)-4007.

### The evidence

The in-repo evidence was circular: the only statement of the clear-on-read
behavior was the comment in `ina228.h` whose correctness was the question, and it
cited "Table 7-9", which is a different register. The datasheet was therefore
fetched and read directly: TI INA228 SLYS021A, January 2021, revised May 2022,
§7.6.1.12, Table 7-16, "DIAG_ALRT Register Field Descriptions". Quoted verbatim:

| Bit | Field | Clear condition |
| --- | --- | --- |
| 11 | ENERGYOF | "Clears when the ENERGY register is read." |
| 10 | CHARGEOF | "Clears when the CHARGE register is read." |
| 9 | MATHOF | "Must be manually cleared by triggering another conversion or by clearing the accumulators with the RSTACC bit." |

Register addresses were checked against the same document's register map
(`ina228.cpp:30-31`, `ina228.h:75`): ENERGY = 9h, CHARGE = Ah, DIAG_ALRT = Bh.
All three match.

### Consequence

Every autonomous record written by the pre-fix firmware reports "no accumulator
overflow", with a correct CRC over that claim, whether or not an overflow
occurred. Nothing errored and nothing was logged — the flag read as clean because
the evidence was cleared microseconds earlier. This is a wrong belief, not a
crash, which is the class that is found at the worst possible time.

Experiment 3's 3,272 stored records are all affected in this respect. Their
charge and energy values are unaffected: the accumulators themselves are read
correctly and before any reset (D-020), and the 40-bit registers are nowhere near
overflow at roughly 1.5 mAh per minute. The defect is that the record cannot
*attest* to that, not that the data is known to be wrong.

`MATHOF` is genuinely reachable — a read does not clear it. Separately, the
datasheet says MATHOF clears on "triggering another conversion", and this
firmware runs the INA228 continuously, so `AUTO_FLAG_INA_MATHOF` qualifies
approximately the latest conversion rather than the whole interval. That is a
measurement-semantics question, it is newly recorded, and the reorder does not
address it.

### A second finding

`readRegister16(REG_DIAG_ALRT, ...)` at :3990 is the **only** `DIAG_ALRT` read in
the firmware. `closeMeasurementInterval()`
[:2786](../Arduino/solar-logger/solar-logger.ino#L2786) — every tethered interval
close — never reads it, so `CSV_DATA` rows in `data/intervals.csv` carry no
overflow qualification at all. A gap rather than a false claim, since the CSV
schema has no overflow column.

### The fix

`readRegister16(REG_DIAG_ALRT, diag)` moved above `readAccumulatedCharge_mAh()`
and `readAccumulatedEnergy_mWh()` in `runAutonomousWakeCycle()`. One call moved.
The flag-setting logic is byte-for-byte the same, the record format is untouched,
and no version was bumped.

Nothing is lost by reading it earlier, and this was checked rather than assumed:
reading `DIAG_ALRT` clears none of the three bits (Table 7-16 lists clear
conditions only for the threshold bits and CNVRF, and only when ALATCH = 1), the
firmware never writes `DIAG_ALRT` so ALATCH stays at its reset value 0
(Transparent), and a grep found no reader of CNVRF or any threshold bit anywhere
in the firmware.

**Two side effects, recorded rather than absorbed:**

1. If both the `DIAG_ALRT` read and the accumulator reads fail in one wake, the
   two error lines now print in the opposite order. Cosmetic, in a double-failure
   path.
2. The `[TIMING]` breakdown bucket `INA validate+accum read` is now
   `INA validate+diag+accum`. The `DIAG_ALRT` read used to fall in the *snapshot*
   bucket, because `tAccumRead` was sampled before it. So that pair of timing
   numbers is **not comparable across this change**, and the label was updated to
   say what it now covers rather than left to mean something other than it says.

**What the fix does not do.** It does not make the flag trustworthy. It makes it
reachable. An overflow has never been induced on this board, so `INA_ACCUM_OF` has
never been observed set, before or after. And stored Experiment 3 records keep
their `INA_ACCUM_OF = 0` from the broken path, where **zero means "not measured",
not "no overflow"**.

### Tests added for the ordering

`test_characterization_record.py` now pins, inside `runAutonomousWakeCycle()`,
that `DIAG_ALRT` is read before both accumulators; that the two flag-setting
bodies are unchanged; and that D-020's read-before-reset still holds.

The order is the correctness, so it is asserted rather than commented. **The guard
was verified to have teeth:** run against the pre-fix committed source, the
`DIAG_ALRT` call sits at in-function offset 2450 against CHARGE at 1905 and ENERGY
at 1969, so both new assertions fail on it. A guard that passes on the bug it
describes is worse than none.

### What changed here

- `solar-logger.ino` — the reorder, the timing label, and comments. The comment at
  the read site used to say the ordering was safe; it now says the order is the
  correctness and points at the test.
- `ina228.h` — corrected "Table 7-9" to Table 7-16, quoted the real clear
  conditions, and stated the obligation any future accumulator reader inherits.
- `tests/test_characterization_record.py` — golden record fixture, binary
  compatibility tests, ordering guards.
- `STORAGE_SYNC_DESIGN.md` §4 wake ordering and §5 record table and flag table,
  `DECISIONS.md` D-020, `BACKLOG.md`, `PROJECT.md`, `INDEX.md`.

### Gate result

`tools/check.sh`: **ALL OK** at every stage — baseline, after the docs/comment
pass, and after the fix.

| | baseline | after docs+comments | after the fix |
| --- | --- | --- | --- |
| pytest | 249 passed, 1 xfailed | 249 passed, 1 xfailed | **333 passed, 1 xfailed** |
| flash | 1,102,455 (84%) | 1,102,455 | 1,102,455 |
| globals | 36,332 (11%) | 36,332 | 36,332 |

The one `xfail` throughout is the known short-cadence autonomous overrun, unrelated.
84 tests were added, all host-only.

**On the flash figure.** The docs+comments pass could not change the image, because
comments do not reach the code generator; identical size there confirms it was a
comment-only edit. The fix stage is different: code moved and a string literal grew
by one character, so **the image is certainly not identical even though its size
is** — the size is quantized by alignment padding, which absorbed the difference.
Equal size is not equal bytes, and this row should not be read as though it were.

### Not done

The `record_format` extraction was **not started**; no `record_format.h` / `.cpp`
exists. `closeMeasurementInterval()` still reads no overflow flag at all. MATHOF's
continuous-conversion semantics and the charge/energy sign asymmetry are both
recorded in BACKLOG and unaddressed. Nothing was uploaded, so the board is still
running the pre-fix image.

### What the record-format extraction will need — established while scoping it

Recorded so the next stint does not rediscover it. All four were read out of the
source, not assumed.

**The characterization tests survive the move without edits.**
`tests/firmware_source.py` reads every file Arduino compiles —
`SOURCE_SUFFIXES = {.ino, .h, .hpp, .c, .cpp}` in the sketch directory plus
`src/` recursively — so the 23 record assertions in
`test_characterization_record.py` and the `static_assert(sizeof(AutoRecord) == 72,`
assertion at `test_autonomous_accounting.py:302` keep finding the struct after it
moves into `record_format.h`. That is the property the module was built for.

**Two of the requested binary-compatibility tests were blocked, then unblocked
within the stint — see the golden-record section below.** They needed real record
bytes, which `LOGGER STORAGE DUMP` does not print directly. A dump line was then
supplied from the board, and it turned out to be sufficient: the dump prints every
field except `magic` and `version` (fixed for any valid record), and prints the
snapshot fields scaled but at exactly microunit resolution, so all 72 bytes are
recoverable and the printed `crc=` checks the recovery. Both tests now exist.

### Golden record from Experiment 3 — the 72-byte layout and the CRC convention are now PROVEN

A real `LOGGER STORAGE DUMP` line was captured from the deployed board and is now
a test fixture. This is the most durable result of the stint.

```text
[STORAGE] #3033 seq=4445 boot=26 exp=3 elapsed_ms=58629900 interval_ms=60004
V=13.051367 I_mA=-0.616 P_mW=8.025 T_C=18.695 dQ_uAh=-10 dE_uWh=129
Qsum_uAh=380771 Esum_uWh=5602351 time=UNKNOWN epoch=0 flags=0x0[none]
crc=0xFB25DA73
```

Rebuilt from `FROZEN_LAYOUT` as little-endian packed bytes, the 68 covered bytes are:

```text
a5b501005d11000063cf0500000000002f7c550000000000030000001a0000000c9f7e03
00000000 64ea0000 e725c700 98fdffff 591f0000 07490000 f6ffffff 81000000
```

and the full 72 bytes end `...8100000073da25fb`, the stored CRC little-endian at
offset 68.

**`zlib.crc32` of those 68 bytes is `0xFB25DA73` — exactly what the board stored.**
It matched on the first attempt, with no search over layout variants.

**Why one number settles so much.** The CRC covers all 68 preceding bytes, so it
reproduces only if every field's offset, width, signedness and byte order matches
the firmware that wrote it, the struct is genuinely packed with no padding, and the
covered range really is bytes 0..67. Any one of those being wrong changes the
checksum. Source reading could establish the *declared* layout; this establishes
the *deployed* one.

**The CRC convention is settled, and the documentation's claim was right.**
`esp_rom_crc32_le(0, buf, len)` is standard IEEE 802.3 CRC-32 — reflected
polynomial `0xEDB88320`, initial value `0xFFFFFFFF`, final XOR `0xFFFFFFFF` —
which is `zlib.crc32`. The ROM function pre-inverts its seed internally, which is
why a `0` seed there corresponds to no seed in zlib. Three other plausible
conventions were computed and all three disagree with the board:

| Convention over bytes 0..67 | Result |
| --- | --- |
| reflected 0xEDB88320, init 0xFFFFFFFF, xor 0xFFFFFFFF (= `zlib.crc32`) | **0xFB25DA73 — matches** |
| init 0, xor 0 (no inversion) | 0xEBF2B4DE |
| init 0xFFFFFFFF, xor 0 | 0x04DA258C |
| init 0, xor 0xFFFFFFFF | 0x140D4B21 |

The previous entry in this stint listed "IEEE 802.3" as an unverified claim
inherited from the docs. It is now verified, and the distinction mattered: three
of the four candidates are wrong.

**Tests added** to `tests/test_characterization_record.py`, host-only, no firmware
code moved: the 72-byte length, the golden CRC, CRC stored little-endian at offset
68, magic/version against the frozen constants, **all 68 covered bytes individually
mutated and each one breaking the CRC**, a flipped stored CRC failing validation,
a guard that the CRC does *not* cover its own field, the snapshot fields traced
back to the printed transcript so the fixture is auditable rather than fitted, and
a check that the two genuinely-negative fields are negative.

That last one matters: a signed field flipped to unsigned packs identically for
positive values, so a golden record of all-positive numbers would not catch it.
`avg_current_uA = -616` and `interval_charge_uAh = -10` on the real board, so the
CRC test does catch it.

**Flag-name decoding cannot move mechanically.** `printAutoRecord()`
[solar-logger.ino:3320](../Arduino/solar-logger/solar-logger.ino#L3320) interleaves
the flag tests with `Serial.print()` and comma bookkeeping, so there is no
decode step to lift out. Extracting the vocabulary means rewriting it into a
buffer-returning helper, which risks the output vocabulary and order the
extraction is supposed to preserve. The clean split is struct, constants,
`autoRecordCrc()`, `autoRecordValid()` and `autoRecordSnapshotUnread()` into
`record_format`, leaving `printAutoRecord()` in the sketch as the console concern
it is.

## 2026-09-23: Telemetry extracted into a module - NOT hardware validated

**No hardware was touched.** Nothing was uploaded or committed, Experiment 3 was
not reset, storage was not cleared, and no serial port was opened. Every result
below is a host test, a source audit, or a compile. **This image has not run on
the board.**

The fourth modularization stage, and stage 5 of the order in
[BACKLOG.md](BACKLOG.md). The machine-readable CSV formatting moved into
`Arduino/solar-logger/telemetry.h` and `telemetry.cpp`. Runtime behavior is
intended to be unchanged, and nothing below found a change. The boundary is
D-051: the module owns how a line is spelled, and never when one is emitted.

### What moved, and what deliberately did not

Six telemetry responsibilities were found in the sketch. Five moved whole, each
renamed to say which line it builds. One was split, because it mixed
measurement policy and acquisition with the row construction:

| Split | Moved to the module | Stayed in the sketch |
| --- | --- | --- |
| `printLiveSample()` | the `CSV_SAMPLE` row construction, as `telemetryPrintSample()` | the `sleepPowerTestRunning` suppression (D-012), the `readSensor()` call and its error line, and the derivation of `elapsed_seconds` from `completedInterval` and `millis()` |

| Was | Is now |
| --- | --- |
| `printCsvHeader()` | `telemetryPrintHeader()` |
| `printCsvRow()` | `telemetryPrintInterval()` |
| `printExperimentStartEvent()` | `telemetryPrintExperimentStart()` |
| `printExperimentResumeEvent()` | `telemetryPrintExperimentResume()` |
| `printSleepTestEvent()` | `telemetryPrintSleepTestEvent()` |

`CSV_HEADER` moved with `CSV_DATA`, although the module table gave the row only
the three data lines: the header names the data row's fields and the two are one
fact. The blank line and the `[CSV] Machine-readable interval logging enabled.`
line that have always preceded it moved with it, so both call sites emit the
same three lines as before.

Nothing else moved. The interval close, the experiment lifecycle, the power-test
state machine, the accounting-suspension predicate, and the command protocol all
stayed. `CMD_ACK`, `CMD_RESULT` and the bounded protocol writer were not touched,
and the 100 ms protocol deadline and `Serial.setTxTimeoutMs(0)` are unchanged.

### Four hidden globals became parameters

`printCsvRow()` read `experimentId`, `runningCharge_mAh` and
`runningEnergy_mWh` out of the sketch. All three event printers read
`experimentId`, and `printExperimentResumeEvent()` also read
`completedInterval`. Those four globals are all passed in now. No global was
exported, and no `extern` was added. This is the D-050 rule applied again.

The two row types, `TelemetrySample` and `TelemetryInterval`, list their members
in wire order. They repeat the four measurement fields rather than taking a
`SensorReading`, which keeps the module's only dependency on Arduino, as the
module table requires. The alternative was thirteen positional arguments, seven
of them `double`.

### The move was audited as a move

Same method as the three earlier stages, over the tokenizer in
`tests/firmware_source.py`, comments stripped:

- The sketch's tokens equal `HEAD`'s with one 388-token run deleted - exactly
  the five function definitions and nothing else - plus the `CSV_SAMPLE` print
  chain inside `printLiveSample()`, and 10 call sites renamed with their new
  explicit arguments. The only insertion that is not an argument is
  `#include "telemetry.h"`.
- **Four of the five moved functions have token-identical bodies**:
  `telemetryPrintHeader`, `telemetryPrintExperimentStart`,
  `telemetryPrintExperimentResume` and `telemetryPrintSleepTestEvent`. The last
  is identical despite gaining a parameter, because the body already printed
  `experimentId` in that position.
- **`telemetryPrintInterval` is identical after substituting the struct field
  for the former global or loose parameter**, and nothing else.
- **Every emitted `Serial` call matches, in order, with its precision
  argument**: 3, 2, 4, 6 and 26 calls across the five, and 12 for the
  `CSV_SAMPLE` chain. The one difference in the whole comparison is a name: the
  sketch's local `experimentElapsedSeconds` is the struct's `elapsedSeconds`.
  Same position, same precision, same value, still computed in the sketch.

The audit script was a scratch file for this stint and is not in the repository.

### The tests

`tests/test_characterization_telemetry.py`, 23 tests, freezing the wire format.
It compiles the module's real source on the host against a recording `Serial`,
runs it, and asserts the exact lines for one fixed set of inputs; then it feeds
those same lines to `app/solar_logger.py`'s own `parse_sample()`,
`parse_interval()` and `parse_event()`.

**The golden lines were captured from the monolith before the move.** The suite
was written first, run green against unmodified `HEAD` source, and then run
green again after the extraction with `GOLDEN` and `INPUTS` untouched. Three
things in the file did change with the move, and all three are plumbing rather
than contract: which function names the harness extracts, how the driver calls
them, and one constant, `TELEMETRY_SOURCE`, naming the file every CSV line is
built in. A fourth change is an assertion that legitimately follows the code -
the interval close now builds a `TelemetryInterval` before emitting it, and the
test pins that ordering against the running totals and the NVS checkpoint.

One limit is stated in the file rather than left implicit: the stand-in
`Serial` carries a copy of `Print::printFloat` from the ESP32 core 3.3.11,
because Arduino does not format a double the way `printf` does - it adds half of
the last requested place and truncates digit by digit. The copy differs from the
device only where `unsigned long` width matters, which is past the `ovf` cutoff
both apply at 4294967040.0, and every asserted value is far inside it.

Telemetry had **no test coverage of any kind** before this stint: no test in the
suite mentioned a CSV prefix or any of the six functions.

**The existing suite was not edited.** 226 passed and 1 xfailed before the move
and after it; the run with the new module is 249 passed, 1 xfailed. The xfail is
the unrelated overrun defect.

### The gate

`tools/check.sh` reported ALL OK before the move and after it.

```text
pytest             249 passed, 1 xfailed        (226 + 23 new)
pyright            0 errors, 0 warnings, 0 informations
py_compile         PASS
firmware compile   clean, --warnings all, no warnings
intellisense       5 translation units, all verified
shell syntax       PASS
whitespace         PASS
```

**The editor found the new module by itself**, the second time the D-048
contract has been exercised by a module created after it was written.
`git diff tools/ .vscode/` is empty.

```text
[INTELLISENSE] Project translation units found (5):
[INTELLISENSE]   connection.cpp
[INTELLISENSE]   ina228.cpp
[INTELLISENSE]   nvs_persistence.cpp
[INTELLISENSE]   solar-logger.ino
[INTELLISENSE]   telemetry.cpp
[INTELLISENSE]   telemetry.cpp: compiles cleanly: ALL OK
                 (direct includes: telemetry.h, Arduino.h)
```

### Where the 118 bytes went

Measured with `size -A` and `nm -S` on both images, built from a clean worktree
at `d017f7d` and from the current tree. Flash 1,102,337 -> 1,102,455.

**Global variables are unchanged at 36,332 bytes**, which is what "the module
owns no mutable state" looks like in the link map. `.flash.rodata` is unchanged
too, so no diagnostic string was duplicated. `.flash.text` +118 is the whole
delta; every other changed section is debug info, which is not flashed.

`.flash.text`, per symbol. This build has no LTO, so a call that used to be
inside one translation unit can no longer be inlined:

| Group | Delta |
| --- | ---: |
| the four functions that moved whole and unchanged | 0 |
| `printCsvRow` 458 -> `telemetryPrintInterval` 360 | -98 |
| `printLiveSample` 346 -> 216, plus `telemetryPrintSample` 180 | +50 |
| call sites now passing what they used to leave implicit | +166 |
| **total** | **+118** |

- `telemetryPrintHeader` (56), `telemetryPrintExperimentStart` (50),
  `telemetryPrintExperimentResume` (80) and `telemetryPrintSleepTestEvent` (108)
  are each **byte-for-byte the same size** as the function they replaced.
- **`telemetryPrintInterval` got 98 bytes smaller.** Reading thirteen fields
  through one struct pointer beats three globals at absolute addresses plus
  seven loose parameters.
- `closeMeasurementInterval()` +128 is that struct being filled on the stack
  before the call, which is where those 98 bytes went and a little more.
- The remaining +38 is six call sites passing `experimentId` or
  `completedInterval` explicitly instead of the callee loading the global:
  `setup()` +12, `stopAllPowerTests()` +8, `enterSleepPowerTestDeepSleep()` +8,
  `armSleepPowerTest()` +8, `resetExperiment()` +4, `armWifiPowerTest()` -2.

### Firmware identity unchanged

```text
[FIRMWARE] Version:  0.3.0-dev
[FIRMWARE] Build ID: solar-logger-protocol-ack-v3
```

A move changes no command, record, cadence rule or protocol, so the version
policy calls for no bump. The revision that `tools/upload.sh` injects will
differ, and that is what identifies this source.

### Hardware acceptance - PENDING, nothing below has been run

This stage changes what reaches `app/solar_logger.py`, so the check is whether
the host still parses what the board sends. After a deliberate
`tools/upload.sh`, send each command on its own and expect exit 0 from each.
Everything after "PREDICTED" is read from the source, not observed.

1. The smoke test in [BACKLOG.md](BACKLOG.md). PREDICTED: a real revision hash,
   `Tail status: INTACT` with at least as many records as before, and
   `CMD_RESULT,...,OK` for HOLD, KEEPALIVE and RELEASE.
2. Stage 5's own check, with `uv run python app/solar_logger.py` attached.
   PREDICTED: `CSV_HEADER` at boot with its thirteen field names unchanged, one
   `CSV_SAMPLE` per second parsed without a field-count warning, and one
   `CSV_DATA` at the interval close. A `[WARNING] CSV_... field count is wrong`
   from the host is the failure this stage would produce, and it would be loud.
3. `data/samples.csv` and `data/intervals.csv` gain rows under their **existing**
   headers. PREDICTED: no header-mismatch exit from `open_csv_file()`, which
   fails closed on a changed schema (D-005). That check is the host's own and
   was not modified.
4. One `CSV_EVENT`. PREDICTED: `EXPERIMENT_RESUME` at boot carrying the current
   experiment id and completed-interval count, written to `data/events.csv` with
   the interval number in the `detail` column.
5. Compare one captured `CSV_DATA` row against a row captured before the upload.
   PREDICTED: same field count, same column order, same decimal places in each
   column - 3 for elapsed seconds, 6 for V/mA/mW and the averages, 4 for
   temperature, 9 for the charge and energy columns.

## 2026-09-23: Documentation audit and supplied hardware validation addendum

**Evidence, not a new hardware run.** This audit read the attachments in
[Audit BMW Logger Project](chatgpt-conversation://6a972711-ff64-83e8-ad38-d78bc4c0e22a)
and compared them with repository revision `d017f7d`, current source, tests and
tooling. No serial port was opened, no upload was performed, Experiment 3 was
not reset and storage was not cleared. Older entries below retain their
original end-of-stint status; “pending” there is historical and is superseded
only by the specific observations recorded here. Discovery-time source line
links can move during modularization; named functions remain the lookup key.

### Observed in supplied hardware transcripts

- INA228 image: clean `4a096d9`; this supplies transcript evidence for the
  earlier reported smoke-test addendum. Connection image: clean `8776ba0`,
  `0.3.0-dev`, build ID `solar-logger-protocol-ack-v3`. The connection trace
  shows NONE before claim, USB claimed on HOLD, session status, successful
  KEEPALIVE and RELEASE, and USB release before sleep.
- NVS image: clean `d017f7d`, same version/build ID. After upload, `STATUS`
  restored Experiment 3, completed interval 52, charge **6.068727 mAh**, energy
  **83.561586 mWh**, with accounting ACTIVE.
- Its storage snapshot: **3,272 valid 72-byte records**, log **235,584 bytes**,
  sequences **1412–4683**, **0 trailing bytes**, tail **INTACT**. Filesystem:
  **1,441,792 total / 245,760 used / 1,196,032 free bytes**. The displayed
  16,611-record / 11-day remainder is a free-space estimate, not a fill test or
  a live reading. Storage was not full at this observation.
- Follow-up POWER TEST STATUS: neither test armed; INA228 continuous conversion.
  HOLD, KEEPALIVE and RELEASE each received machine ACK and `CMD_RESULT,...,OK`.
  RELEASE closed a real **1.757 s** interval and wrote schema **2**, experiment
  **3**, interval **53**, charge **6.068421 mAh**, energy **83.565454 mWh**,
  followed by `NVS CHECKPOINT SAVED SUCCESSFULLY`.
- RELEASE armed the 250 ms grace, delivered its OK result, and reported
  **59,750 ms** remaining before sleep; the host classified the following
  disconnect as expected. This confirms the corrected D-046 RELEASE path on
  hardware. It is a firmware-reported commanded remainder, not an independent
  measurement of actual sleep duration or clock accuracy.
- Next autonomous status: ON, armed YES, running YES, cadence **60 s**, boot
  **29**, RTC valid YES, next sequence **4746**, reserved through **4804**, and
  new-record experiment context **3**. Tethered `STATUS` on that unclaimed
  timer wake printed zero experiment/interval/totals with accounting SUSPENDED.
  Source explains why: the early wake path reads experiment context separately
  and bypasses the tethered checkpoint load. Those zeroes do not establish NVS
  loss, nor does this status independently reread the new interval-53 checkpoint.
- Claimed-wake traces include measurement-work timings: **51.438 ms** on
  `8776ba0` and **42.376 ms** on the NVS follow-up. These are individual reported
  spans, not an unattended wake budget, a before/after performance comparison,
  or a breakdown of LittleFS costs.

The small negative charge change on interval 53 accompanies measured negative
current; monotonically increasing charge was an incorrect acceptance criterion.
Energy rose. The snapshot establishes preservation of Experiment 3 at that time,
not its current interval or totals while the device continues running.

### What remains unproven or unimplemented

No explicit no-keepalive lease-expiry trace was found in the reviewed
attachments. That path shares tested scheduler arithmetic, but the RELEASE
observation does not prove it independently. Pending failure-injection checks,
long-session timing, full storage, RTC drift, and full unattended wake/power
characterization remain open. The NVS restore/write smoke does not exercise NVS
failure handling or a cold-boot reread of the newly saved interval 53.

Sync, storage ACKs, acknowledged-sequence state, reclamation, ring storage and
SET_TIME are absent. The 10-second-cadence boundary/accumulator overrun issue
remains the one strict `xfail`. The two NVS read-default defects remain open.
A separate DIAG_ALRT ordering contradiction is recorded as a source question in
BACKLOG, not a proven hardware overflow failure.

### Local validation and documentation changes

Fresh host run in this audit: `PYTHONDONTWRITEBYTECODE=1 .venv/bin/python -B -m pytest -p no:cacheprovider` — **226 passed, 1 xfailed in 38.48 s**. This includes
the real IntelliSense scratch-sketch scenario; it does not open serial. No new
tests or runtime changes were made.

The supplied `d017f7d` transcript, rather than a new full gate run by this audit,
records `tools/check.sh` ALL OK: 226 passed / 1 xfailed, pyright 0 errors / 0
warnings, py_compile pass, clean Arduino compile with `--warnings all`, shell
checks and whitespace pass. IntelliSense discovered and compiled four project
units automatically: `connection.cpp`, `ina228.cpp`, `nvs_persistence.cpp`, and
`solar-logger.ino`. Check build: 1,102,337 bytes flash, 36,332 bytes globals;
revision-injected upload build: 1,102,281 bytes flash, same globals.

The canonical validation command remains `tools/check.sh`; uploads use
`tools/upload.sh`, serial commands use `tools/send.sh`, and editor staleness is
checked with `tools/intellisense.sh --check`. KEEPALIVE/RELEASE never wait;
explicit `--wait` on either is refused. A queued command through the running
logger has no completion result in the sender, even if its exit status is 0.

The audit corrects current status, old echo-based instructions, partition usage,
record-field semantics, storage-full wording and historical proposal labels in
INDEX, PROJECT, DECISIONS, BACKLOG, STORAGE_SYNC_DESIGN and README. Firmware
identity remains **0.3.0-dev / solar-logger-protocol-ack-v3**; documentation does
not remove the firmware's dev suffix. Telemetry extraction is in progress in a
separate task; this audit neither edits its code nor declares it complete.

## 2026-09-23: NVS persistence extracted into a module - NOT hardware validated

**No hardware was touched.** Nothing was uploaded or committed, Experiment 3
was not reset, storage was not cleared, and no serial port was opened. Every
result below is a host test, a source audit, or a compile. **This image has not
run on the board.**

The third modularization stage, and stage 1 of the order in
[BACKLOG.md](BACKLOG.md). The mechanics of ESP32 nonvolatile storage moved into
`Arduino/solar-logger/nvs_persistence.h` and `nvs_persistence.cpp`. Runtime
behavior is intended to be unchanged, and nothing below found a change.

The module is named for NVS rather than "persistence" because this board has
three durable stores with different promises - NVS, RTC-retained memory, and the
LittleFS record log - and only the first one moved. The boundary is D-050.

### What moved, and what deliberately did not

Fifteen NVS responsibilities were found in the sketch. Thirteen functions moved
whole. Two were split, because each mixed a typed NVS access with a decision
that is not the module's to make:

| Split | Moved to the module | Stayed in the sketch |
| --- | --- | --- |
| `loadAutonomousSettings()` | the two-key read, as `loadAutonomousConfig()` | the cadence range check, with `AUTO_INTERVAL_SECONDS_MIN` / `_MAX` (D-018) |
| `reserveSequenceBlock()` | the write, as `saveSequenceHighWater()` | `AUTO_SEQ_BLOCK`, and the RTC mirror `rtcAutoSeqHighWater` |

`ensureSequenceReservation()` stayed whole: it decides *whether* a reservation
is needed. Nothing about which of the durable log tail and the NVS reservation
is authoritative moved (D-023).

### One semantic difference, found by the audit and guarded

`loadAutonomousSettings()` used to `return true` early when the namespace would
not open, which **skipped** the range check below it. That early return is now
inside `loadAutonomousConfig()`, so it comes back to the caller and the range
check runs on the default instead of being skipped.

The two are identical only while the default cadence is itself in range. It is,
60 against 10-3600, so no behavior changes - but a later cadence change could
break it silently, printing "out of range" at every boot. A `static_assert` in
the sketch now fails the build instead. This is the only place where the
extraction is not behavior-preserving by construction.

### Two known defects were preserved, not fixed

Both are recorded in [BACKLOG.md](BACKLOG.md) under "NVS load failures become
plausible defaults with no signal", and both are unchanged by this move:
`loadAutonomousConfig()` still reports success after a failed open, and
`loadSequenceHighWater()` still returns 0, which reads the same as "nothing was
ever reserved". Fixing either changes behavior and needs its own change. They
are now stated in `nvs_persistence.h` where a caller will see them.

### The move was audited as a move

Same method as the two earlier stages, over the tokenizer in
`tests/firmware_source.py`, comments stripped:

- The sketch's tokens equal `HEAD`'s with 2,215 tokens deleted in 17 runs and
  90 inserted. Every insertion is an explicit argument or result at a former
  hidden dependency, plus `#include "nvs_persistence.h"` and the
  `static_assert` above.
- **Eight functions have byte-for-byte identical bodies and parameter lists**:
  `loadExperimentIdOnly`, `saveAutonomousTestArmed`, `saveAutonomousInterval`,
  `loadSequenceHighWater`, `nextBootId`, and the three power-test savers.
- **Five are identical after substituting the sketch global for its new out
  parameter**, and nothing else: `saveCheckpoint`, `loadCheckpoint`, and the
  three power-test loaders.
- **Both split functions recompose exactly.** Inlining `saveSequenceHighWater()`
  back into `reserveSequenceBlock()`, and `loadAutonomousConfig()` back into
  `loadAutonomousSettings()`, reproduces each `HEAD` body token for token.
- The namespace string, all seven key constants, `NVS_SCHEMA_VERSION` and the
  `Preferences` object are identical, with `static` added.

The audit script was a scratch file for this stint and is not in the repository.

### The tests

`tests/test_characterization_nvs.py`, 43 tests, freezing what is stored: the
`solarlog` namespace, all twelve key names and the 15-character ESP32 limit,
the five checkpoint keys with their writer, reader and checked width, the
`LoggerCheckpoint` field types, failure propagation on both checkpoint paths,
the power-test defaults, the sequence key, and the rule that the RTC floor
rises only after the NVS write succeeded.

Two of them are deliberately file-specific, because they are the boundary
rather than the shape of a function: exactly one file may use `preferences`,
and the sketch may not name a key, the namespace, or the object.

Not duplicated: that a failed experiment-id read cannot become experiment 0.
`tests/test_autonomous_accounting.py` already pins it (D-024), and it still
passes unchanged after the move.

**Nine mutations were applied one at a time and all nine were caught**: the
namespace renamed, a checkpoint key renamed, `charge` written with `putUInt`
instead of `putDouble`, a failed key write abandoning the remaining keys, an
unsupported schema returning true, the RTC floor raised before the NVS write,
the Wi-Fi flag defaulting to armed, the sketch naming a key again, and the
reservation written to the `boot_id` key.

**The existing suite was not edited.** 183 passed and 1 xfailed before the move
and after it; the run with the new module is 226 passed, 1 xfailed. The xfail is
the unrelated overrun defect.

### The gate

`tools/check.sh` reported ALL OK before the move and after it.

```text
pytest             226 passed, 1 xfailed        (183 + 43 new)
pyright            0 errors, 0 warnings, 0 informations
py_compile         PASS
firmware compile   clean, --warnings all, no warnings
intellisense       4 translation units, all verified
shell syntax       PASS
whitespace         PASS
```

**The editor found the new module by itself.** `nvs_persistence.cpp` got a
verified entry with no edit to `tools/intellisense.py`, `tools/check.sh`, or any
VS Code setting, which is the D-048 contract and the first time it has been
exercised by a module created after it was written. `git diff tools/ .vscode/`
is empty.

```text
[INTELLISENSE]   nvs_persistence.cpp: compiles cleanly: ALL OK
                 (direct includes: nvs_persistence.h, Arduino.h, Preferences.h)
```

### Where the 820 bytes went

Measured with `nm`, `size -A` and `objdump -h` on both images, and on the object
files of both builds. Flash 1,101,517 -> 1,102,337. **Global variables are
unchanged at 36,332 bytes**: the `Preferences` object is the same 8 bytes under
a new name, `_ZL11preferences`.

The whole delta is accounted for:

| Section | Delta |
| --- | ---: |
| `.flash.text` | +248 |
| `.flash.rodata` | +416 |
| `.eh_frame` | +152 |
| `.flash.init_array` | +4 |
| **total** | **+820** |

`.flash.text`, per symbol. This build has no LTO, so a call that used to be
inside one translation unit can no longer be inlined:

- The three checkpoint call sites carry the struct now: `setup()` +232,
  `closeMeasurementInterval()` +58, `resetExperiment()` +14.
- **The moved functions themselves got smaller**, because writing through a
  reference beat writing four separate globals: `loadCheckpoint` 1,122 -> 1,034
  and `saveCheckpoint` 618 -> 574. The three power-test loaders lost 2 bytes
  each.
- The two splits cost +54 and +16 for the extra call.
- Static initialization is now emitted in two translation units instead of one,
  +12.

`.flash.rodata` +416 is the second object file's own read-only fragments and
their alignment; at object level it is +459, so the linker's string merging
reclaimed 43 of it. **No diagnostic string was duplicated** - the set of
`[NVS]`, `[ERROR]` and `[AUTO]` literals in the two images is identical.
`.eh_frame` +152 is unwind data for the new function set. The 4 bytes of
`.flash.init_array` are one more static constructor slot.

### Firmware identity unchanged

```text
[FIRMWARE] Version:  0.3.0-dev
[FIRMWARE] Build ID: solar-logger-protocol-ack-v3
```

A move changes no command, record, cadence rule or protocol, so the version
policy calls for no bump. The revision that `tools/upload.sh` injects will
differ, and that is what identifies this source.

### Hardware acceptance - PENDING, nothing below has been run

This stage touches the store that holds Experiment 3, so the first check is
whether the board comes back as the same experiment. After a deliberate
`tools/upload.sh`, send each command on its own and expect exit 0 from each.
Everything after "PREDICTED" is read from the source, not observed.

1. The smoke test in [BACKLOG.md](BACKLOG.md). PREDICTED: a real revision hash,
   `Tail status: INTACT` with at least as many records as before, and
   `CMD_RESULT,...,OK` for HOLD, KEEPALIVE and RELEASE.
2. Stage 1's own check, and the one that matters most here: the boot log's
   `[NVS]` block. PREDICTED: `Stored schema = 2`, `experiment = 3`, and the
   same interval number and running charge/energy the board reported before the
   upload. An `experiment = 0` or a zeroed interval means the checkpoint was
   not read and the board must not be left running.
3. `tools/send.sh LOGGER AUTONOMOUS STATUS`. PREDICTED: the same armed state and
   the same 60-second cadence as before the upload, both read from NVS, and
   `New records will carry experiment id: 3`.
4. `tools/send.sh POWER TEST STATUS`. PREDICTED: both tests not armed, which is
   the persisted state, and no `[NVS] ... unavailable` line at boot.
5. One closed interval, tethered: PREDICTED `[NVS] CHECKPOINT REQUIRED` followed
   by the five `[NVS] Writing` lines and
   `*** NVS CHECKPOINT SAVED SUCCESSFULLY ***`, with the interval number one
   higher than step 2 reported.
6. A cold boot with autonomous mode armed. PREDICTED: `boot_id` increments by
   one across the reboot, and `[STORAGE] Sequence reservation already covers
   <n>` rather than a fresh block, since the existing reservation still covers
   the next sequence.

## 2026-09-18: Connection bookkeeping extracted, and the editor database finds every module - NOT hardware validated

**No hardware was touched.** Nothing was uploaded or committed, Experiment 3
was not reset, and storage was not cleared. Every result below is a host test,
a source audit, or a compile. **This image has not run on the board.**

Two changes, in this order: the IntelliSense tooling now configures every
project module by discovery (D-048), and then the second modularization stage
moved the transport bookkeeping into `Arduino/solar-logger/connection.h` and
`connection.cpp` (D-049). Runtime behavior is intended to be unchanged, and
nothing below found a change.

### Why the editor could not find Arduino.h in ina228.cpp

Read out of the generated database, not inferred. Arduino CLI compiles copies
of the sketch files under `<build>/sketch/`, and its compilation database
names the copies. The only entry for the driver was for
`build/intellisense/sketch/ina228.cpp`, and its ESP-IDF include path was still
behind the `@response` / `-iprefix` indirection D-039 describes. Nothing named
`Arduino/solar-logger/ina228.cpp`, and cpptools matches entries by path, so the
file a person edits had no configuration at all. `tools/intellisense.py` added
an editor entry for the `.ino` only.

### A second defect found in the same tool: the "previous configuration" was gone

Observed in a scratch copy of the sketch with the tool as it was at `HEAD`. When
verification failed, it printed:

```text
[INTELLISENSE] ERROR: The sketch does NOT compile under the flags the editor is
about to be given (exit 1, output above). The database has NOT been written, so
the previous configuration is still in place.
```

The database file then held 80 entries and no entry for the `.ino`. Arduino CLI
writes its raw database to its build path, and the build path was the directory
the editor reads from, so the previous editor database had been overwritten
before verification ran. PROJECT.md made the same claim. Arduino CLI now
builds into `build/intellisense/arduino-cli/`; see D-048.

### Arduino CLI's behavior, measured on arduino-cli 1.5.1

A scratch copy of the sketch with a probe file of each kind:

- The database has an entry for `.c`, `.cpp`, `.cc`, `.cxx` and `.S` files in
  the sketch root and anywhere under `src/`. A `.cpp` in any other
  subdirectory is copied into the build and gets no entry.
- `.c` and `.S` are compiled with `riscv32-esp-elf-gcc`, the rest with `g++`.
- Deleting a module removes both its build copy and its database entry on the
  next run. Arduino CLI leaves no stale entry behind.
- `--only-compilation-database` took 9.0 s on a new build path, 8.4 s after a
  file changed, and 1.5 s with nothing changed.
- 2 of the 335 expanded include directories do not exist in the installed SDK
  (`esp_system/port/soc` and an `espressif__esp_matter` path). The core lists
  them anyway, so the tool cannot require every include directory to exist.

### What the tooling does now

- The project's translation units are read out of Arduino CLI's database:
  every entry for a copy under `<build>/sketch/`, mapped back to the real file.
  No module is named anywhere. A scan of the sketch root and `src/` must agree.
- Each one gets exactly one entry naming the real file, with every response
  file and `-iprefix` expanded, and it replaces the build copy's entry. The
  `.ino` also gets `-x c++ -include Arduino.h`; a `.cpp` gets neither.
- Before writing, every entry must resolve `Arduino.h` and the four ESP-IDF
  headers from D-039, and every file is compiled `-fsyntax-only` with exactly
  its entry's flags, in parallel.
- `tools/check.sh` runs `tools/intellisense.sh` as a step. Adding a module and
  running the gate is the whole procedure.
- `--check` reports a stale entry for a deleted file, a missing entry for a new
  one, and any change to a source or header, and names the command to run.

In a scratch copy with `.c`, `.cc`, `.cxx`, `.S`, a nested `src/sub/` module and
a `.cpp` outside `src/`, the tool configured and verified the six C and C++
units, printed a NOTE that the `.S` file gets no editor entry, and left the
other subdirectory alone.

### The generator's output for this repository

After `connection.cpp` was created, with no configuration edit of any kind:

```text
[INTELLISENSE] Project translation units found (3):
[INTELLISENSE]   connection.cpp
[INTELLISENSE]   ina228.cpp
[INTELLISENSE]   solar-logger.ino
[INTELLISENSE] Every project entry resolves Arduino.h, freertos/FreeRTOS.h, esp_sleep.h, soc/soc_caps.h, esp_rom_crc.h: ALL OK
[INTELLISENSE]   connection.cpp: compiles cleanly: ALL OK (direct includes: connection.h, Arduino.h)
[INTELLISENSE]   ina228.cpp: compiles cleanly: ALL OK (direct includes: ina228.h, Arduino.h, Wire.h)
[INTELLISENSE]   solar-logger.ino: compiles cleanly: ALL OK (direct includes: ... ina228.h, connection.h)
```

Regeneration took 12.5 s on a new build path and 2.6 s on a warm one.
`--check` took 0.07 s.

**Not observed: what VS Code shows.** The database is proven by compiling every
entry. Whether cpptools applies it to `ina228.cpp` and `connection.cpp` has to
be seen in the editor. The clang diagnostics Claude Code displayed for
`connection.cpp` while it was being written came from a clangd process that
Claude Code itself started, not from VS Code, so they say nothing about what
cpptools shows.

### What moved into connection

- `activeTransports`, now `static` in `connection.cpp`. Only
  `connectionClaim()` and `connectionRelease()` write it.
- `CONNECTION_NONE`, `CONNECTION_USB`, `CONNECTION_WIFI`, `CONNECTION_BLE`.
  Only `CONNECTION_USB` is in the header, because nothing else outside the
  module uses the others (D-047).
- `anyHostConnected()`, `printActiveTransports()`, `connectionClaim()`,
  `connectionRelease()`, exported, and `transportName()`, now `static`.
- The two forward declarations the sketch kept for `anyHostConnected()` and
  `printActiveTransports()`. `connection.h` declares them now, so the block is
  seventeen entries.

What stayed in the sketch, deliberately (D-049):

- Everything about the lease: HOLD, KEEPALIVE, RELEASE, expiry, the lease
  globals, `hostClaimedRendezvous`, the deferred sleep.
- `printUsbPresence()` and the rendezvous's `USB plugged` / `CDC connected`
  lines. They report electrical presence, which is not connection state.
- Every call site. HOLD claims USB, RELEASE and lease expiry release it,
  `LOGGER AUTONOMOUS OFF` releases it only with no session held, and sleep
  policy asks `anyHostConnected()`. All unchanged.

`solar-logger.ino` went from 7,949 to 7,836 lines by `wc -l`: 118 removed and 5
added. `connection.h` is 50 lines and `connection.cpp` 120.

### Validation performed

| Check | Result |
| --- | --- |
| `tools/check.sh` before any change | `ALL OK`: 153 passed, 1 xfailed; pyright 0 errors, 0 warnings, 0 informations; compile clean |
| `tools/check.sh` after the tooling change, before the move | `ALL OK`, seven steps including the new `intellisense` step: 183 passed, 1 xfailed; pyright 0/0/0; compile clean; image unchanged |
| `tools/check.sh` after the move | `ALL OK`, seven steps: 183 passed, 1 xfailed, the same overrun `xfail`; pyright 0/0/0; compile clean with `--warnings all`, no warnings; intellisense covers all three translation units |
| firmware image | 1,101,475 → 1,101,517 bytes (84%, +42) |
| globals | 36,332 → 36,332 bytes (11%, unchanged) |
| untracked files | `connection.h`, `connection.cpp` and both new test files: no trailing whitespace and no space before a tab |

The 30 new tests are in two files. `tests/test_characterization_connection.py`
was written and passed against the monolith before the move, then passed
unchanged after it. `tests/test_intellisense.py` holds one scenario test that
runs the real generator, Arduino CLI and toolchain against a scratch copy of the
sketch, and nineteen fast tests of the validator, the copy-to-source mapping and
the fingerprint. The scenario took 26.6 s, and it is why pytest went from 2.3 s
to 30.2 s.

### The tests were verified to fail

- **Connection, 10 of 10 mutations caught**, run once against the monolith and
  once after the move: release clearing every bit, `anyHostConnected()`
  asking about USB only, HOLD no longer claiming, the `Host claimed` wording,
  `LOGGER AUTONOMOUS OFF` releasing a held session's USB, the `+` separator,
  the maintenance window trusting `Serial.isConnected()`, booting with USB
  claimed, the claim line's wording, and the BLE bit colliding with Wi-Fi.
  Reformatting every brace K&R and editing comments left the suite green.
- **IntelliSense, both defects restored and caught** by the scenario test:
  only the `.ino` getting an entry, and the database written before
  verification. The second one first passed. The refused database held the
  same bytes as the previous one, so an unchanged file also fit a rewrite. The
  test now marks the previous file with trailing whitespace first, which the
  generator never writes.

### The move was audited as a move

Same method as the INA228 stage, over the tokenizer in
`tests/firmware_source.py`, comments stripped:

- The sketch's tokens equal `HEAD`'s with four runs deleted, 350 tokens in
  total, and `#include "connection.h"` inserted. Nothing else changed.
- All 340 moved tokens are in the new files. The 46 added tokens are
  `#pragma once`, three includes, two `static`, and the four declarations in
  `connection.h`.
- All five functions have identical bodies and parameter lists. All four
  constants and the `activeTransports` definition are identical, with
  `static` added to the state.

The audit script was a scratch file for this stint and is not in the repository.

### Where the 42 bytes went

Measured with `nm`, `size -A` and `objdump` on both images. `.flash.text`
+38 and `.eh_frame` +4; the debug sections do not reach flash.

- `anyHostConnected()` had no symbol before: it was inlined into every caller
  in the same translation unit. It is now a 4-instruction, 14-byte function,
  called three times from `setup()` and once each from the rendezvous, the
  maintenance window and `printHostSessionStatus()`. Those last three became
  one instruction shorter, 4 bytes each. `setup()` kept 789 instructions and
  grew 2 bytes; that was not examined further.
- `transportName()` was a separate 58-byte function called by claim and
  release. As a `static` it was inlined into both: `connectionClaim()` went
  from 34 to 48 instructions and `connectionRelease()` from 52 to 66, 46 bytes
  each.
- `hostSessionHold()`, `hostSessionRelease()`, `serviceHostLease()` and
  `stopAutonomousTest()` have the same size and instruction count, and call the
  same connection functions as before.
- The 4 bytes of `.eh_frame` were not examined.

### Firmware identity unchanged

```text
[FIRMWARE] Version:  0.3.0-dev
[FIRMWARE] Build ID: solar-logger-protocol-ack-v3
```

A move changes no command, record, cadence rule or protocol, so the version
policy calls for no bump. The revision that `tools/upload.sh` injects will
differ, and that is what identifies this source.

### Hardware acceptance - PENDING, nothing below has been run

After a deliberate `tools/upload.sh`, send each command on its own and expect
exit 0 from each. Everything after "PREDICTED" is read from the source, not
observed.

1. The smoke test in [BACKLOG.md](BACKLOG.md). PREDICTED: a real revision hash,
   `Tail status: INTACT` with at least as many records as before, and
   `CMD_RESULT,...,OK` for HOLD, KEEPALIVE and RELEASE.
2. Stage 2's own check, run during the HOLD from step 1: `tools/send.sh LOGGER
   SESSION STATUS`. PREDICTED: `[CONNECTION] Active transports: USB` and
   `[CONNECTION] Any host connected: YES`. HOLD's own output PREDICTED:
   `[CONNECTION] USB transport CLAIMED by an explicit host lease.`
3. RELEASE's output. PREDICTED: `[CONNECTION] USB transport RELEASED.`,
   `[CONNECTION] Active transports: NONE`, `[CONNECTION] Any host connected:
   NO`, then the result, then the transport disappearing.
4. An unclaimed rendezvous, read with `tools/watch-port.sh` or a terminal on
   the port. PREDICTED, once per second: `[CONNECTION] Active transports: NONE`
   and `[CONNECTION] Host claimed: NO`, next to whatever `USB plugged` and
   `CDC connected` report.
5. Lease expiry, optional: HOLD and send nothing. PREDICTED: `[CONNECTION] USB
   transport RELEASED due to lease expiry.` and the same three lines as
   step 3.

## 2026-09-18: INA228 driver extracted into a module - NOT hardware validated

### Addendum, reported later the same day: the INA228 image was smoke-tested

**Reported in the connection stint's brief, and recorded here from it. Not
observed by the agent that wrote this addendum.** The brief states that the
image built from `4a096d9` was uploaded and checked on hardware, and gives
these results:

- `VERSION`: `0.3.0-dev`, revision `4a096d9` with no `-dirty`,
  `solar-logger-protocol-ack-v3`.
- `LOGGER STORAGE INFO`: `Tail status: INTACT`, more than 2,000 valid 72-byte
  records, about 12 days free at the 60-second cadence.
- `LOGGER AUTONOMOUS STATUS`: on, armed, running, 60-second interval, RTC state
  valid.
- `LOGGER SESSION HOLD`, `KEEPALIVE` and `RELEASE`: `CMD_RESULT,...,OK` for
  each. RELEASE closed the tethered interval, saved the NVS checkpoint, armed
  the deferred sleep with about 59,750 ms remaining, and the transport
  disappeared after the RESULT.
- `STATUS` after the next wake: valid live voltage, current and power.
- `POWER TEST STATUS`: the INA228 in continuous conversion.
- Recent autonomous records: `interval_ms` of 60,004, consecutive sequences,
  cumulative charge and energy that reconcile, and no invalid records.

That covers steps 1 to 3 of the pending list below, and part of step 4: the
brief does not mention the records' flags or a nonzero `dQ_uAh`. Step 5 is not
mentioned. The RELEASE figure is also the first check listed under the
scheduler entry below, which predicted a remainder just under 59,750 ms.

**Original extraction-stint status (before the smoke-test addendum above):**
no hardware was touched, nothing was uploaded or committed, Experiment 3 was
not reset and storage was not cleared. The extraction results below are host
tests, a source audit and a compile; the image had not yet run on the board at
the time they were recorded.

The first modularization stage. The INA228 driver moved out of
`solar-logger.ino` into `Arduino/solar-logger/ina228.h` and `ina228.cpp`.
Runtime behavior is intended to be unchanged, and nothing below found a change.
The boundary rules are [DECISIONS.md](DECISIONS.md) D-047. The plan's status,
and where this stage departs from it, are in [BACKLOG.md](BACKLOG.md).

### What moved

- **23 functions:** register reads and writes, sign extension, identity,
  configuration, the ADC_CONFIG MODE decode, shutdown and continuous mode,
  `resetInaAccumulators()`, `readSensor()`, `readAccumulatedCharge_mAh()` and
  `readAccumulatedEnergy_mWh()`. Bodies unchanged. The 11 that nothing outside
  the driver calls are now `static`.
- **33 `constexpr` constants.** The 15 that the sketch still uses are in
  `ina228.h`. The 18 that only the driver uses are private to `ina228.cpp`.
- **`struct SensorReading`**, into the header, because `readSensor()` fills it.

What stayed in the sketch:

- `validateInaForWake()` and the wake cycle's DIAG_ALRT read. BACKLOG assigns
  both to `auto_orchestrator`. They reach the part through `readRegister16()`,
  which the header exports for them.
- `Wire.begin()` and `Wire.setClock()` in `setup()` and
  `runAutonomousWakeCycle()`. Bus bring-up is part of boot and wake ordering.
  The header states it as a precondition.
- `inaShutdownActive`. The power test writes it, and STATUS and the heartbeat
  read it. The driver does neither.
- Every decision about when to read, reset, or reconfigure.

`solar-logger.ino` went from 9,126 to 7,949 lines by `wc -l`: 1,184 lines moved
out, 4 added for the include, and one 4-line comment rewritten as 7 because the
move made it false. `ina228.h` is 224 lines and `ina228.cpp` 1,048. Git's
default diff misaligns this change; `git diff --diff-algorithm=histogram` shows
it as 1,188 deletions and 11 additions.

### Validation performed

| Check | Result |
| --- | --- |
| `tools/check.sh` before the move | `ALL OK`: 153 passed, 1 xfailed; pyright 0 errors, 0 warnings; compile clean |
| `tools/check.sh` after the move | `ALL OK`: 153 passed, 1 xfailed, the same overrun `xfail`; pyright 0 errors, 0 warnings, 0 informations; compile clean with `--warnings all`, no warnings |
| firmware image | 1,102,133 → 1,101,475 bytes (84%, −658) |
| globals | 36,332 → 36,332 bytes (11%, unchanged) |
| `ina228.h`, `ina228.cpp` | untracked, so `git diff HEAD --check` does not cover them; checked separately, no trailing whitespace and no space before a tab |
| `tools/intellisense.sh` | the raw `.ino`, which now includes `ina228.h`, compiles standalone with the editor's flags: ALL OK; 81 entries |

No test was edited. The source tests read every sketch file (D-043), so they
found the moved code without changes.

### The move was audited as a move

With the tokenizer from `tests/firmware_source.py`, which drops comments, so
code is compared and prose is not:

- The sketch's tokens equal the committed sketch's with three runs deleted and
  `#include "ina228.h"` inserted. No remaining line of code changed.
- No token of the committed sketch is missing from the three files. The only
  tokens added are `#pragma once`, five includes, eleven `static`, and the
  twelve declarations.
- All 23 functions have identical bodies and parameter lists. Each header
  declaration matches its definition.
- All 33 constants have identical definitions. Every constant in `ina228.h` is
  used by the sketch, and none private to `ina228.cpp` is referenced by it.

The audit script was a scratch file for this stint and is not in the repository.

### Where the 658 bytes went

Measured with `nm` and `objdump` on both images: `.flash.text` −454 and
`.eh_frame` −204.

- The build has no LTO, so sketch code can no longer inline driver functions
  across the file boundary: `closeMeasurementInterval()` −182,
  `runAutonomousWakeCycle()` −90, `setup()` −72.
- Five helpers that are now `static` no longer exist as separate functions,
  because the compiler inlined them into their callers inside `ina228.cpp`:
  `readRegister20Unsigned`, `readRegister24Unsigned`, `readRegister40Signed`,
  `signExtend20` and `inaModeIsShutdown`. `readSensor()` grew 60 bytes and now
  calls `readRegisterBytes()` where it used to call the first two. Its
  soft-float library calls are the same operations in the same counts before
  and after. `resetInaAccumulators()` and `readAccumulatedCharge_mAh()`, the
  two callers of `readRegister40Signed`, grew 12 bytes each.
- `inaModeFromAdcConfig()` had been inlined at every call. It now also exists
  once as a 4-byte function.
- The other moved functions shrank by 2 to 6 bytes or did not change.
  `verifyInaIdentity()` was disassembled: the same 84 instructions, with
  different string addresses and branch offsets.
- Seven unrelated functions grew 2 bytes each. `connectionClaim()` and
  `hostLeaseRemainingMs()` were disassembled: the same instructions, with
  different addresses and branch offsets. The other five were not examined.
- `printHeartbeat()` grew 18 bytes and `printLiveSample()` shrank 14. In both,
  the return after a failed `readSensor()` is laid out differently. Each calls
  the same set of functions before and after.

### Found, not fixed

**The editor database has no entry for `ina228.cpp` itself.** Arduino CLI's
entry names the build copy, `build/intellisense/sketch/ina228.cpp`, and
`tools/intellisense.py` adds an editor entry only for the `.ino`. BACKLOG said
`.cpp` modules would get correct entries for free, and they do not. Predicted,
not observed in the editor: cpptools will not apply the ESP32 include paths to
`ina228.cpp`. The real build is unaffected. Recorded in
[BACKLOG.md](BACKLOG.md).

### The hardware baseline this stint relied on is not recorded here

The stint brief says the monolith (0.3.0-dev, `solar-logger-protocol-ack-v3`)
was checked on hardware before this extraction. It quotes an autonomous
`interval_ms` of about 60,004, 2,022 records read with 0 invalid, a consecutive
tail, and a first sleep after RELEASE of about 59,750 ms. No entry in this file
records that run, and the entries below still list their acceptance as pending.
The figures are quoted from the brief. They are not a recorded observation.

### Firmware identity unchanged

```text
[FIRMWARE] Version:  0.3.0-dev
[FIRMWARE] Build ID: solar-logger-protocol-ack-v3
```

A move changes no command, record, cadence rule or protocol, so the version
policy calls for no bump. The revision that `tools/upload.sh` injects will
differ, and that is what identifies this source.

### Hardware acceptance - PENDING, nothing below has been run

After a deliberate `tools/upload.sh`, send each command on its own and expect
exit 0 from each. Everything after "PREDICTED" is read from the source, not
observed.

1. The smoke test in [BACKLOG.md](BACKLOG.md): `VERSION`,
   `LOGGER STORAGE INFO`, `LOGGER AUTONOMOUS STATUS`, then `LOGGER SESSION
   HOLD`, `KEEPALIVE` and `RELEASE`. PREDICTED: a real revision hash, a record
   count at least as high as before the upload with `Tail status: INTACT`, and
   `CMD_RESULT,...,OK` for all three session commands.
2. `tools/send.sh STATUS`. PREDICTED: `[STATUS] Voltage`, `Current` and `Power`
   lines with live values, and `CMD_RESULT,STATUS,OK`.
3. `tools/send.sh POWER TEST STATUS`. PREDICTED:
   `[POWER TEST] INA228 mode right now: continuous conversion`.
4. After at least two unclaimed timer wakes, `tools/send.sh LOGGER STORAGE
   DUMP`. PREDICTED: the records written since the upload have `interval_ms`
   near 60,000, a nonzero `dQ_uAh`, and none of `ACCUM_SUSPECT`, `INA_MATHOF`
   or `INA_ACCUM_OF`. The first carries `FIRST_AFTER_BOOT`, because the upload
   was a cold boot. `ACCUM_SUSPECT` is what a failed `validateInaForWake()`,
   accumulator read or DIAG_ALRT read would set.
5. Only if a host is reading the port during the cold boot that follows the
   upload. PREDICTED: `[INA228] Identity check: PASS` and
   `[INA228] CONFIG / ADC_CONFIG / SHUNT_CAL readback: PASS`. Output with
   nobody reading is dropped (D-027), so its absence proves nothing.

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
