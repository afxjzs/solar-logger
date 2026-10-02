# Current state — BMW Solar Logger

Updated **2026-10-02**, after the `16a299d` hardware acceptance, the cadence
change, and the reviewed-but-uncommitted D-063 retained-state extraction. Read
the canonical documents in the order required by
[AGENTS.md](../AGENTS.md), then verify fresh Git/source and hardware ownership.
Navigation: [INDEX.md](INDEX.md).

This file is the compact checkpoint. Dated history lives in
[LAB_NOTES.md](LAB_NOTES.md), decisions in [DECISIONS.md](DECISIONS.md), open
work in [BACKLOG.md](BACKLOG.md). It was rewritten on 2026-10-02 because it had
accreted into a log; do not let it do that again.

## Next action

**Commit the D-063 retained-state extraction, then upload and run the full
hardware acceptance.** The extraction is built, host-tested and
orchestrator-reviewed; it is **not committed and not uploaded**. BACKLOG requires
more than the short extraction smoke for anything touching retained state: run
the complete [2026-09-17 acceptance sequence](LAB_NOTES.md#2026-09-17-correctness-stint---six-must-fix-defects-resolved-first-tests-added),
including lease expiry (step 7), five consecutive unclaimed records (step 8) and
the refusals (steps 9 and 10). The 2026-10-02 LAB_NOTES entry lists the exact
transcript lines that would show retained state still works, and the failure
signature if it does not.

After that, the next modularization candidate is **sequence reservation**:
`AUTO_SEQ_BLOCK`, `reserveSequenceBlock()` and `ensureSequenceReservation()` as
their own module, which can now include `autonomous_retained_state.h`. D-063
settled who owns the mirror, which is what was blocking it.

Nothing is blocked.

## Repository and hardware snapshot

Canonical path: `/Users/afxjzs/dev/projects/solar-charger`. HEAD **`65f55d1`** on
main, **12 commits ahead of origin/main; no fetch and no push.** Index empty.

Two separate pieces of uncommitted work, and they must not be committed together:

- **The D-063 retained-state extraction**, built 2026-10-02 and reviewed the
  same day. New `Arduino/solar-logger/autonomous_retained_state.{h,cpp}` and
  `tests/test_characterization_autonomous_retained_state.py`; modified
  `solar-logger.ino` (one `#include`, the definitions removed, three comment
  fixes), `sequence_authority.h` (comment only), and the DECISIONS, LAB_NOTES,
  BACKLOG and CURRENT_STATE entries for it. Ready to commit.
- **The capture-gap stage** (D-059): `tools/charger-transitions.py`,
  `tests/test_charger_transitions.py`, and the pandas and tzdata entries in
  `pyproject.toml` / `uv.lock`. Complete and host-tested, and still awaiting a
  commit-or-drop decision after several days.

**The board runs `16a299d`**, verified by `VERSION` on 2026-10-02:
`0.3.0-dev / 16a299d / solar-logger-protocol-ack-v3`. Experiment 3 is intact,
sequences from 1412, board invalid count 0.

**Cadence is 300 seconds** as of 2026-10-02, changed from 60 s to buy storage
headroom. That change cleared `rtcAutoMagic` and re-armed, which produced boot id
42 — a re-arm, not a reset.

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
| Retained autonomous state (D-063) | Built and host-tested 2026-10-02, orchestrator-reviewed, **not committed, not uploaded, not hardware validated** |
| Capture-gap report (D-059) | Implemented and host-tested, **still uncommitted** |
| Software gate, working tree 2026-10-02 | `ALL OK`, exit 0 — **563 passed / 1 xfailed**, flash 1,106,273 B, globals 36,332 B, IntelliSense **9/9**. Run twice: by the CODE stage and independently by the orchestrator |

Sketch: **6,670 lines**. Eight modules extracted. D-063 changed no byte of the
image: ten allocated sections identical in address and size, four differing
bytes in total, all of them the core's compile-time stamp, and identical symbol
tables. Audit ELFs rather than `.bin` files — see the 2026-10-02 LAB_NOTES entry
for why a `.bin` diff shows 69 bytes.

## Latest hardware evidence

[2026-10-02 acceptance of `16a299d`](../logs/evidence/2026-10-01/storage-recovery-16a299d/startup-baseline.md),
covering `6192c2d` and `16a299d` together. Final dump 15,021 records, sequences
1412–16432, no gaps, invalid 0, all `exp=3`; all 13,991 preflight record lines
byte-identical. Running totals and sequence both continued across the upload's
reset. The preceding [2026-10-01 acceptance](../logs/evidence/2026-09-29/storage-recovery-58a4e09/startup-baseline.md)
covered `44d8b27` and `58a4e09`.

**D-062 was observed deciding, not merely running.** During the cadence change,
arming printed `Next sequence from log: 16439, from NVS reservation: 16453,
using: 16439`. The two inputs disagreed by 14 and the module correctly took the
log. Quoted in full in D-062.

Damaged-log branches, a real filesystem fault, induced accumulator overflow,
storage-full behavior and lease expiry all remain **untested on hardware**.

## Storage headroom — the nearest deadline

At the 2026-10-02 acceptance the board reported room for 4,721 more records. At
the new 300-second cadence that is roughly **16 days, to about 2026-10-18**.
There is still no storage-full policy; see BACKLOG. Raising the cadence buys
weeks, not a fix, and the capacity table in STORAGE_SYNC_DESIGN §6 assumes a
fresh log, so prefer the board's own `LOGGER STORAGE INFO` projection.

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
