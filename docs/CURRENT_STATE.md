# Current state — BMW Solar Logger

Updated **2026-10-05**, after the D-063 retained-state extraction passed its full
hardware acceptance on 2026-10-02. Read
the canonical documents in the order required by
[AGENTS.md](../AGENTS.md), then verify fresh Git/source and hardware ownership.
Navigation: [INDEX.md](INDEX.md).

This file is the compact checkpoint. Dated history lives in
[LAB_NOTES.md](LAB_NOTES.md), decisions in [DECISIONS.md](DECISIONS.md), open
work in [BACKLOG.md](BACKLOG.md). It was rewritten on 2026-10-02 because it had
accreted into a log; do not let it do that again.

## Next action

**The sequence-reservation extraction.** `AUTO_SEQ_BLOCK`,
`reserveSequenceBlock()` and `ensureSequenceReservation()` become their own
module, which includes `autonomous_retained_state.h` rather than needing an
`extern` of its own. D-063 settled who owns the mirror, which is what was
blocking it. Give it a fresh CODE session with a kickoff printed in a single
fenced code block per [CLAUDE.md](../CLAUDE.md).

**That kickoff must state that no existing test goes red on a relocation.** Both
`tests/firmware_source.py` consumers that touch the mirror —
`test_characterization_nvs.py`'s write-ordering assertion and D-063's writer-set
test — are keyed on **function name across every file Arduino compiles**, so
moving `reserveSequenceBlock()` under the same name leaves them green. Measured
2026-10-02 by performing the move in a scratch copy, not inferred. So the stage
gets no red test for free and needs its own positive assertions, the way D-063's
single-definition and `extern`-only-header tests were its real evidence.

The nearest real deadline is unrelated to modularization: **storage fills around
2026-10-17** and there is still no storage-full policy. See BACKLOG for the cheap
version already costed out.

Nothing is blocked.

## Repository and hardware snapshot

Canonical path: `/Users/afxjzs/dev/projects/solar-charger`. Work on `main`, and
**`origin/main` holds the whole history** as of this update — the 12 commits that
had never left this machine were pushed on 2026-10-02. Verify with
`git rev-list --left-right --count origin/main...HEAD` rather than trusting this
line.

Three commits landed on 2026-10-02, deliberately kept separate:

- **`1801098`, the D-063 retained-state extraction.** New
  `Arduino/solar-logger/autonomous_retained_state.{h,cpp}` and
  `tests/test_characterization_autonomous_retained_state.py`; modified
  `solar-logger.ino` (one `#include`, the definitions removed, three comment
  fixes) and `sequence_authority.h` (a corrected docstring and comment, no
  assertion changed). **Hardware validated 2026-10-02.**
- **`8faa0e3`, the capture-gap stage** (D-059): `tools/charger-transitions.py`,
  `tests/test_charger_transitions.py`, and the pandas and tzdata entries in
  `pyproject.toml` / `uv.lock`. It had sat uncommitted for days while D-059,
  PROJECT and INDEX already described it as existing, so the docs referenced a
  file the repository did not contain. Committing it closed that gap. Host-only;
  it opens no serial port and reads only CSV.
- **The acceptance record**, including
  `logs/evidence/2026-10-02/retained-state-1801098/` and the
  `tools/storage-smoke.py` wait-timeout fix the acceptance itself turned up.

**The board runs `8faa0e3`**, verified by `VERSION` on 2026-10-02:
`0.3.0-dev / 8faa0e3 / solar-logger-protocol-ack-v3`. Experiment 3 is intact,
sequences from 1412, board invalid count 0.

**Why the image reports `8faa0e3` and not `1801098`.** `tools/upload.sh` stamps
`git rev-parse --short HEAD`, and `8faa0e3` sits on top of the firmware change
without touching firmware. The content flashed is `1801098`'s.

**Cadence is 300 seconds** as of 2026-10-02, changed from 60 s to buy storage
headroom. That change cleared `rtcAutoMagic` and re-armed, which produced boot id
42 — a re-arm, not a reset. Last observed board state, 2026-10-02 16:15: armed,
boot id 44, RTC state valid, cycles 7, next sequence 16519, experiment 3. **The
board was not re-queried on 2026-10-05** — it was asleep, which is its ordinary
state. Everything above is a dated observation, not a live reading.

## Software versus hardware

| Work | Current evidence |
| --- | --- |
| `ina228`, `connection`, `nvs_persistence`, `telemetry` | Extracted 2026-09-18 and 2026-09-23; see INDEX for each one's evidence and limits |
| `record_format` | Hardware validated on clean `eba3b5d` |
| `storage` | Hardware validated on clean `1980d94` (2026-09-29) |
| Scan-open diagnostic (D-057 addendum) | Committed `44d8b27`, hardware validated 2026-10-01 |
| Running totals from an unread log (D-060) | Committed `58a4e09`, hardware validated 2026-10-01 |
| Retained-state validity (D-061) | Committed `6192c2d`, hardware validated 2026-10-02 |
| Sequence authority (D-062) | Committed `16a299d`, **hardware validated 2026-10-02** |
| Retained autonomous state (D-063) | Committed `1801098`, **hardware validated 2026-10-02** on image `8faa0e3`. Nine of ten relocated variables directly evidenced; see D-063 for the tenth, which this transport cannot observe |
| Capture-gap report (D-059) | Implemented and host-tested, committed 2026-10-02. Host analysis only; it opens no serial port |
| Software gate | `ALL OK`, exit 0 — **563 passed / 1 xfailed**, flash 1,106,273 B, globals 36,332 B, IntelliSense **9/9**. Run three times at this tree: by the CODE stage, by the orchestrator on 2026-10-02, and again on 2026-10-05 after the tooling fix. Identical every time |

Sketch: **6,670 lines**. Eight modules extracted. D-063 changed no byte of the
image: ten allocated sections identical in address and size, four differing
bytes in total, all of them the core's compile-time stamp, and identical symbol
tables. Audit ELFs rather than `.bin` files — see the 2026-10-02 LAB_NOTES entry
for why a `.bin` diff shows 69 bytes.

## Latest hardware evidence

**[2026-10-02 acceptance of `1801098`](../logs/evidence/2026-10-02/retained-state-1801098/acceptance-result.md)**,
the D-063 retained-state extraction, on image `8faa0e3`. Criteria were written
before any result, in `startup-baseline.md` beside it. Final dump 15,106 records,
sequences 1412–16517, **zero gaps across the whole history**, invalid 0, all
`exp=3`, and **all 15,091 preflight record lines byte-identical**. Both assertion
tools exit 0 with `--compare` against the preflight dump, so the continuity
boundary was identified rather than inferred.

The sharpest single result: **`FIRST_AFTER_BOOT` on exactly one record out of six
consecutive unclaimed wakes.** That flag is set when `rtcAutoCycleCount` reads 0,
so a retained definition duplicated across translation units would have set it on
every wake. One flag in six is positive proof of a single definition surviving
five deep sleeps.

The earlier [acceptance of `16a299d`](../logs/evidence/2026-10-01/storage-recovery-16a299d/startup-baseline.md)
covered `6192c2d` and `16a299d`, and the one before it `44d8b27` and `58a4e09`.

**D-062 has now been observed deciding three times**, each with its inputs in
genuine contention: `16439` against a floor of `16453` during the cadence change,
then the floor at `16516` declined after the upload's reset, then
`Next sequence from log: 16512, from NVS reservation: 16517, using: 16512` on the
re-arm. Applying the floor in any of them would have left a hole in a log the
board itself reports INTACT.

**Lease expiry is no longer untested.** The 2026-10-02 acceptance holds the
project's first hardware lease-expiry trace: `Host lease EXPIRED after 15000 ms
without keepalive`, the firmware releasing the transport itself, the open
tethered interval closing, and a full-cadence sleep of 300,000 ms against
`RELEASE`'s 299,750 ms — the D-046 asymmetry, observed side by side for the first
time.

Damaged-log branches, a real filesystem fault, induced accumulator overflow and
storage-full behavior all remain **untested on hardware**.

## Storage headroom — the nearest deadline

Last **measured** figure, from the 2026-10-02 acceptance: room for **4,664 more
records**, which the board itself projected as 16 days at the 300-second cadence.

**Computed forward from there, not measured:** about 830 records by 2026-10-05,
leaving roughly 3,800, or **about 2026-10-17**. The board was asleep when this
was written and was not queried, so treat the date as arithmetic rather than
evidence and run `LOGGER STORAGE INFO` for a real figure.

There is still no storage-full policy; see BACKLOG for the cheap version already
costed out. Raising the cadence buys weeks, not a fix, and the capacity table in
STORAGE_SYNC_DESIGN §6 assumes a fresh log, so prefer the board's own projection.

## Open findings not yet fixed

All recorded in BACKLOG with reasons:

- Re-arming after `AUTONOMOUS OFF` discards running totals that were still good.
- Three unexplained resets on 2026-10-01, before any operator command.
- A command can attach to a rendezvous that is almost over and be lost.
- Long diagnostic output is silently truncated; HELP is the usual casualty.
- Generic `STATUS` reports uninitialized experiment zeros on a timer wake.
- No storage-full policy.
- Power-test extraction deferred until its lower layers move. It would need
  **upward calls** into Wi-Fi control, autonomous ownership and the accounting
  predicates, which is the part that still blocks it. The `extern` half of that
  objection is spent: D-063 established the project's first one, deliberately and
  downward, so a later module may declare its own state rather than reach up into
  the sketch.

## Hardware and installation

Identities, the **traced** measurement path and the sign convention are canonical
in [HARDWARE_WIRING.md](HARDWARE_WIRING.md); the phase structure is in
[PROJECT.md](PROJECT.md). In short: panel and MPPT controller are one unit, the
shunt sits in the positive leg between controller and battery, **positive current
means charging into the battery**, and the enclosure holds only the XIAO, the
INA228 and the shunt.

Still open: the 12 V supply stage, whether a connection at the under-hood
charging posts stays observable to the IBS, which node `VBUS` senses, the
enclosure itself, and OFF-edge LED correlation.

**Do not re-ask** for the INA breakout purchase listing, the SUNER model, or
whether Python was running during the overnight run. All are recorded.

**Do not power the INA228 down merely to extinguish its indicator LED** —
accumulation through sleep is intentional. No LED modification has been made and
no disable jumper has been located.

## Key entry points

- `tools/check.sh` — the canonical gate; never uploads, never opens a port.
- `tools/upload.sh`, `tools/send.sh` — the only supported device access.
- `tools/capture-command.py` — wraps one hardware command and preserves its
  output, argv, exit status and SHA-256. Refuses to overwrite a label, and
  refuses destructive commands.
- `tools/storage-smoke.py`, `tools/validate-dump.py`,
  `tools/check-totals-continuity.py` — drive and judge a hardware acceptance.
- `Arduino/solar-logger/sequence_authority.{h,cpp}` — the cleanest seam in the
  project: a pure function with no `extern` and no upward call. Still the model
  when a stage moves a *decision*.
- `Arduino/solar-logger/autonomous_retained_state.{h,cpp}` — the newest module,
  and the model when a stage moves *state* rather than a decision: one definition
  of each value in one translation unit, exported `extern`, and no transition.
  Its header explains why the `extern` is a named departure from D-047 and what
  test compensates for it.
- `data/samples.csv` — the only wall-clock reference in the project. Durable
  records carry `epoch=0` and `time=UNKNOWN`. Experiment 3 began
  `2026-09-16T10:07:44`.

## Checkpoint maintenance

Update this file after a meaningful change or handoff. Refresh the Git snapshot
and the validation scope; link evidence and detailed history rather than copying
it here. Record new user facts immediately in the hardware register or lab notes,
and keep unknowns explicit. Future agents follow AGENTS.md without needing any
chat history.
