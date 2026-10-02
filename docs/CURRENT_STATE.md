# Current state — BMW Solar Logger

Updated **2026-10-01** by CODE stages, in the two dated sections directly below
only. The
rest of this file is still the 2026-09-29 checkpoint. Read canonical documents in the order required by
[AGENTS.md](../AGENTS.md), then verify fresh Git/source and hardware ownership.
Navigation: [INDEX.md](INDEX.md).

## 2026-10-01 — sequence authority extracted (D-062), uncommitted

**Compiled and host-tested in the working tree on top of HEAD `6192c2d`. NOT
committed, uploaded or hardware validated.** The seventh module,
`sequence_authority.h/.cpp`, holds only the D-023 decision,
`sequenceAuthorityNext(scan, reservedHighWater)`. Reservation, `AUTO_SEQ_BLOCK`,
the RTC mirror `rtcAutoSeqHighWater` and the totals reseed stayed in the sketch.
The user chose that boundary.

- Stage files: the two new module files,
  `tests/test_characterization_sequence_authority.py`, `solar-logger.ino`,
  comment-only edits in `storage.h/.cpp`, and one harness line in
  `tests/test_characterization_storage_recovery.py`. Commit them separately
  from other stages' work.
- Combined-tree gate: before, 499 passed / 1 xfailed; after, `ALL OK` with
  **529 passed / 1 xfailed**, flash +78 B to 1,106,273, globals unchanged,
  IntelliSense 8/8. No isolated gate was run.
- Next: orchestrator review, a commit, then a deliberate healthy-log hardware
  smoke. Details are in the newest LAB_NOTES entry.

## 2026-10-01 — retained-totals validity (D-061), uncommitted

**Implemented and host-tested in the working tree. NOT committed, uploaded or
hardware validated.** Two files: `Arduino/solar-logger/solar-logger.ino`
(`autoStorageRecover(bool retainedStateValid)` and its four call sites) and
`tests/test_characterization_storage_recovery.py`. With an unreadable log,
recovery now carries the retained totals if `rtcAutoMagic` was valid before the
call site set it, and zeroes them otherwise. Both cases are announced.

- Red-check against staged `58a4e09`: 5 failed, 1 passed (healthy log).
- Isolated gate (`58a4e09` plus the two files): `ALL OK`, **475 passed / 1
  xfailed**, flash 1,106,195 B (+488), globals 36,332 B.
- Combined working tree: `ALL OK`, **499 passed / 1 xfailed**.
- Cold-boot resume reachability is settled from the source in LAB_NOTES.
- Not reconciled here: the kickoff states `58a4e09` passed healthy-log hardware
  acceptance on 2026-10-01 (13,650 records, 1412–15061, invalid 0). No canonical
  doc records it yet. Evidence appears to be under
  `logs/evidence/2026-09-29/storage-recovery-58a4e09/`, which this stage did not
  read or touch.
- Next: orchestrator review and a separate commit of the two files, then a
  healthy-log hardware smoke.

## Next action (2026-09-29)

**The CODE_KICKOFF open-for-scan fix is reviewed, red-checked and COMMITTED as
`44d8b27`. It has NOT been uploaded or hardware validated.** Four files, +296/-17:
`Arduino/solar-logger/storage.h`, `storage.cpp`, `solar-logger.ino` and
`tests/test_characterization_storage_recovery.py`. Nothing pushed. The host
capture-gap work and all documentation changes were deliberately left out of
that commit and are still uncommitted in the same tree; do not stage all.

**The next action for this fix is its healthy-log hardware smoke**, described
below. It is the only outstanding step.

A hardware smoke for it is a later, deliberate stage and its only meaningful
check is the **healthy-log** path — that the change did not move what it must not
move. The open-failure branch needs a real filesystem fault and **must not** be
obtained by damaging the Experiment 3 log.

**Two follow-ups this fix deliberately did not take**, both now in BACKLOG:
boot recovery silently restarts the RTC running totals at zero from a log it
could not read, and generic STATUS still reports uninitialized experiment zeros
during a timer-wake rendezvous. Each wants its own bounded stage.

**Update, later on 2026-09-29: the running-totals follow-up is REVIEWED and
COMMITTED as `58a4e09` under D-060. It is NOT uploaded or hardware validated.**
Two files, +102/-4: `Arduino/solar-logger/solar-logger.ino`
(`autoStorageRecover()` only) and `tests/test_characterization_storage_recovery.py`.
The red-check and gate are in the newest LAB_NOTES entries. The STATUS-zero
defect is untouched.

**Review of that commit raised one OPEN issue and corrected one citation.**
Recovery cannot read `rtcAutoMagic` to tell whether the totals it now carries
were ever valid, because every call site overwrites the magic immediately
before calling — new BACKLOG item, "Recovery cannot see whether the retained
totals it carries were ever valid". And D-060's ESP-IDF claim was read from
v5.4 while the installed core bundles **v5.5.5**; the pin is corrected in D-060
and the gap is named there rather than closed.

**Storage commit 1980d94 passed its deliberate healthy-log hardware smoke**
earlier the same day. Experiment 3 is preserved and the board resumed autonomous
logging at 60 s. No further firmware extraction or cause classifier was started.
The host capture-gap report and its dependency/docs changes remain a separate
uncommitted stage.

**Workflow clarified:** this chat is the orchestrator; implementation goes to
fresh, dedicated CODE sessions for each new stage, rather than reusing sessions.
This supersedes the earlier suggestion to reuse an existing CODE session.

[CODE_KICKOFF.md](CODE_KICKOFF.md) is the brief this fix was executed against,
retained as the record of what was asked. Host report/dependency review can
still proceed independently; coordinate shared docs and full-gate inputs before
concurrent edits.

## Repository and ownership snapshot

Canonical path: `/Users/afxjzs/dev/projects/solar-charger`. HEAD **58a4e09**,
main, three ahead of existing origin/main; no fetch and no push. Index empty.

**The firmware and its tests are committed and clean**: the scan-open fix landed
in `44d8b27` as four files, and the D-060 running-totals fix in `58a4e09` as two.
No firmware or test file is dirty. The canonical hardware tools
(`tools/upload.sh`, `tools/send.sh`, `tools/check.sh`) are untouched. Still
uncommitted and belonging to other stages: the host capture-gap report/test, the
pandas dependency changes, README/docs, AGENTS/checkpoint and earlier evidence.
**The documentation describing the scan-open fix is among them** — it is spread
through LAB_NOTES, BACKLOG, DECISIONS, STORAGE_SYNC_DESIGN, INDEX and PROJECT
alongside other stages' prose, which is why it was not committed with the code.
Recheck status before any commit, and commit these stages separately rather than
staging all.

App inventory showed no other active BMW chat. Two Claude Code processes had
this repository as cwd; user explicitly confirmed the CODE sessions idle and
this chat's ownership. No host logger or pending command/upload marker was found.
No logger was started, and the smoke's held session was successfully released.
This is a dated observation, not a lock; recheck before new writes/device work.

## Software versus hardware

| Work | Current evidence |
| --- | --- |
| record_format | Clean eba3b5d hardware validation; unchanged version 1 / 72-byte layout |
| Durable-log recovery | Committed 43c3baf; earlier eba3b5d-dirty healthy-log smoke; also runs in new 1980d94 |
| storage extraction | **Committed and healthy-log hardware-smoke validated on clean 1980d94** |
| Capture-gap report | Implemented/host-tested, still uncommitted; D-059 |
| Scan-open diagnostic fix | **Committed `44d8b27`, reviewed and red-checked; NOT uploaded or hardware validated**; D-057 addendum |
| Scan-open red-check | September 29: detached worktree at `1980d94`, new test file only, `openFailed` declared but never set — **5 of 7 fail, 2 pass**. The 2 that pass are the conservative half. Verbatim pre-fix serial reproduced the rewrite-to-zero-bytes path |
| Isolated storage software gate | September 29, before this fix: **462 passed / 1 strict xfailed** |
| Isolated gate with this fix | `1980d94` plus only its four files: `ALL OK` — **469 passed / 1 strict xfailed**, IntelliSense 7/7 |
| Combined working-tree gate | Before this fix **486 passed / 1 xfailed**; after it `ALL OK` — **493 passed / 1 xfailed**, all steps green |
| D-060 running totals | **Uncommitted, host-tested, NOT uploaded**. Red-check against unmodified `44d8b27`: both new failure-mode tests fail, (0,0) and stale (300,400). Isolated gate (`44d8b27` plus its two files): `ALL OK`, **472 passed / 1 xfailed**, flash 1,105,707 B (+434), globals 36,332 B. Combined working tree: `ALL OK`, **496 passed / 1 xfailed** |
| D-061 retained-totals validity | **Uncommitted, host-tested, NOT uploaded** (2026-10-01). Red-check 5 failed / 1 passed against staged `58a4e09`. Isolated gate **475 passed / 1 xfailed**, flash 1,106,195 B; combined **499 passed / 1 xfailed** |
| Earlier session | Canonical upload build + hardware smoke, complete dump comparison, checksum/source/whitespace checks |

Current board identity verified by VERSION: **0.3.0-dev / 1980d94 /
solar-logger-protocol-ack-v3**. Uploaded build: flash **1,104,265 B**, globals
**36,332 B**; prior uninjected gate was 1,104,313 B. These are different build
identities. No firmware source or version-label change was made.

## Latest board/data evidence

[September 29 complete hardware evidence](../logs/evidence/2026-09-29/storage-hardware/README.md), with raw canonical command
output, argv/timestamps/exit statuses, validation reports and SHA-256 manifests.
Detailed observations/limits: newest LAB_NOTES entry.

- Preflight VERSION: eba3b5d-dirty. Complete dump: **10,659 records, 1412–12070**,
  all exp=3, invalid 0; all 5,756 September 25 record lines match exactly.
- Canonical uploader succeeded once; board VERSION independently confirms clean
  1980d94. Post-upload INFO: **10,661 / newest 12072**, 767,592 B.
- Later autonomous INFO: **10,663 / 12074**, 767,736 B. Both INFO scans: 72-byte
  records, trailing 0, INTACT, no printed storage errors.
- Final dump: **10,664 records, 1412–12075 consecutive**, all exp=3, invalid 0;
  all **10,659** preflight record lines match exactly. END, summary, ACK and
  matching RESULT OK precede normal autonomous sleep/transport loss.
- Boot 36: **12073–12075**, 3 new records, interval_ms 60004; FIRST_AFTER_BOOT
  set on 12073 then clear. 12071/12072 were added by the old image before upload.
- HOLD/KEEPALIVE/RELEASE passed; held NVS/STATUS restored Experiment 3 interval
  **479**. RELEASE saved **480** after 3.788 s and resumed sleep with 59,750 ms
  remaining. New records after RELEASE prove autonomous progress.
- Generic STATUS during early timer wake misleadingly reports uninitialized
  zeros. NVS-backed AUTONOMOUS STATUS and the records prove exp=3; this is tracked
  as a separate diagnostic defect. Immediate cold-boot status before autonomous
  initialization likewise had boot/next-seq 0; later records establish recovery.
- HOLD/RELEASE warned accumulator reset readback ENERGY=0 CHARGE=-1. Preserved,
  not classified as a storage error or proof of a sensor fault/reset timing.
- Board-reported CRC validity is distinct from independent binary verification.
  All epochs are 0/time UNKNOWN; retained time is not exact wall-clock coverage.
  No damaged-log branch, storage-full, real overflow or lease-expiry test was run.

No standalone reset, storage clear, whole-flash erase or corruption injection.
The authorized uploader performed its normal bootloader reset, targeted firmware
programming and restart. Preserve Experiment 3 in every later stage.

Earlier host samples/report remain the September 28 evidence: 26,674 rows,
2 contiguous edges, 39 other state observations, 80 gaps. Hardware smoke did not
start the Python logger or modify host telemetry. The user previously confirmed
Python was stopped during the earlier overnight run; do not re-ask that history.

## SUNER characterization and user observations

- Two 6-ohm 50-W resistors in series (~12 ohm) brought charging back after about
  3.5 minutes; they became scalding hot and were removed. Temperature unmeasured.
- Continuous ON current edge: Sep 24 **14:09:49.468777 −0.628 mA →
  14:09:50.468071 +178.692 → 14:09:51.469187 +262.236** (UTC−07:00).
- Continuous OFF-like current edge: **14:48:16.097899 +62.708 mA →
  14:48:17.099301 −0.624 mA**. No timestamped OFF LED correlation supplied.
- Panel stayed connected overnight in the window. User reports cloudy conditions
  and no charging the following morning. They suggest low light plus a mostly
  topped-up battery may jointly prevent restart. Plausible, **not established**.
- Observed output state and inferred cause must stay separate. Time of day is
  supporting context, not proof of light, connection or battery-full state.
  Keep UNKNOWN/ambiguous/fault possibilities; causes can coexist.
- The old five-entry report conflated state observations with timed edges.
  The corrected report at defaults (2 s max spacing, five samples per state)
  gives **2 contiguous edges, 39 other state observations and 80 capture gaps**.
  Gap/experiment/time-order boundaries restart persistence; ambiguous evidence
  cannot qualify an edge. Context keeps positional indexing and timezone offsets.
  All numeric settings remain provisional characterization choices.
- Production state inference should use **direct INA readings in firmware**.
  CSV is characterization only; no production thresholds chosen. Sleeping ESP
  evaluates only at wakes; no second-resolution transition detection while asleep.

## Hardware identity and LED question

Canonical register: [HARDWARE_WIRING.md](HARDWARE_WIRING.md).
Purchased INA breakout: user-provided **Ubxvamm storefront / “5832 INA228”** listing.
Actual PCB manufacturer and revision remain **unverified/unknown**; product URL or
ASIN not provided. Do not infer Adafruit origin or treat 5832 as a revision.
The screenshot is preserved; do not ask again for the already-known listing.
Nominal shunt 15 mΩ (R015), project calibration 15.62 mΩ.

MCU board: Seeed XIAO ESP32-C3, PCB revision unknown. USB currently powers it;
3V3 powers INA logic, D4/D5 are I2C, external VUSB header unconnected. Physical
SUNER/shunt/battery path and polarity remain untraced; INA placement in the
controller output path is an inference. SUNER exact model also remains unknown.

XIAO onboard LED is a charger indicator, not normal GPIO control. INA LED wiring
needs the actual board circuit; no disable jumper/resistor location verified.
No LED hardware modification or savings measurement performed. Do not power off
INA merely to extinguish a breakout LED; accumulation during sleep is intentional.

## Limits and later work

**The open-for-scan diagnostic defect is FIXED AND COMMITTED (`44d8b27`); no
board has run it.** An existing log that fails open-for-scan now sets
`AutoLogScan.openFailed` and `readError`, so recovery names its tail UNREAD,
`LOGGER STORAGE INFO` prints `Tail status: UNREADABLE` with no fabricated counts
and answers `CMD_RESULT,...,ERROR`, and the rewrite refuses. Sequence recovery
still uses the NVS floor and the log is still never truncated on this outcome,
which was already true and is now pinned by tests. The same regression found
that `autoStorageTruncateToValid()`, called directly, announced a rewrite to
zero bytes for that log and was stopped only by its own open failing; it now
refuses first. If an unexpected UNREADABLE or any storage error appears during a
later smoke, stop acceptance and diagnose. Details: the newest LAB_NOTES entry,
the D-057 addendum and BACKLOG's open-for-scan issue.

**Left open by that fix, deliberately:** `autoStorageRecover()` reseeds the
RTC-retained running totals from a scan that read nothing, so they silently
restart at zero. Same on the pre-existing short-read path. Its own stage.
**Now fixed in the uncommitted working tree (D-060)**: on `openFailed ||
readError` the totals are not assigned, and recovery prints that they could not
be recovered and the values it carries. Not uploaded; see LAB_NOTES.

Sync, storage ACK/reclamation/full policy, time synchronization, BLE/iPhone/server,
manual SYNC circuitry and installed vehicle power remain unimplemented. Installed
plan: under-hood electrical points, glove-box/cabin logger, phone BLE bridge;
trunk/IBS topology remains research. D-052–D-058 and BACKLOG retain details.

Open immediate evidence: OFF LED correlation, exact physical measurement wiring,
PCB revision/manufacturer verification if needed for LED work, and meaning of
retained-time gaps before wall-clock analysis. Do not repeat known purchase
identity or historical overnight host-status questions.

## Documentation reading cautions

Fresh September 29 hardware addenda supersede older extraction-time statements
that storage has not been uploaded. Historical dated counts/images remain valid
for their dates. The generic STATUS early-wake diagnostic remains OPEN in
BACKLOG. The open-for-scan failure is fixed and committed but never uploaded, so
**the board is still running the code that has it**: the uploaded `1980d94`
image predates `44d8b27`, and every hardware transcript on file was produced by
it. Firmware still says 0.3.0-dev; the pre-existing release-label policy
discrepancy remains documented, without a version-only edit.

## Key entry points

- `tools/charger-transitions.py` / `tests/test_charger_transitions.py`: completed host reporting stage, still uncommitted.
- `tests/test_characterization_storage_recovery.py`: synthetic file-backed recovery gate; scan-open coverage committed in `44d8b27`.
- `Arduino/solar-logger/storage.h` / `.cpp`: mechanics extraction committed in 1980d94 and healthy-log hardware-smoke validated, plus the scan-open fix (`AutoLogScan.openFailed`) committed in `44d8b27`, **not uploaded**.
- `Arduino/solar-logger/solar-logger.ino`: sequence authority, RTC and scheduling policy; `printStorageInfo()` unreadable branch committed in `44d8b27`, **not uploaded**.
- `logs/evidence/2026-09-29/scan-open-review/`: review artifacts for `44d8b27`, including `redcheck.sh`, which reproduces the pre-fix failures on demand.
- `tools/check.sh`: full software gate, no upload or serial access.
- `tools/send.sh` / `app/device_session.py`: sleeping-board commands and lease rules.
- `tools/upload.sh`: only supported coordinated uploader; deliberate hardware stage.

## Checkpoint maintenance

Update this file after a meaningful change or handoff; link evidence and detailed
history rather than duplicating transcripts. Refresh Git snapshot and validation
scope. Record new user facts immediately in the hardware register/lab notes;
keep unknowns explicit. Future agents follow AGENTS.md without needing this chat.
