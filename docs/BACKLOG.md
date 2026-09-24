# Backlog

Open work, proposed directions, and resolved findings retained with their status. Accepted architecture decisions live in [DECISIONS.md](DECISIONS.md). Older source line links are discovery-time references and may have moved during extraction; use the named function and the module locations below for current source.

## How this file is organized

- **Current and near-term work** — committed, expected to happen next.
- **Known bugs and issues** — diagnosed defects, with resolved items explicitly marked and retained as history.
- **Experiments to run** — bench measurements that are defined but not yet taken.
- **Longer-term ideas** — promising concepts that are **not** committed work and must not be read as planned.

The boundary that matters most is the last one. Everything under "Longer-term ideas" is speculation with a rationale attached, not a roadmap.

---

# Current and near-term work

## Installed-system work — settled direction, implementation and research pending

The scope is D-052–D-054 in [DECISIONS.md](DECISIONS.md), summarized in
[PROJECT.md](PROJECT.md). Nothing in this section is an installation result.
BLE/iPhone/server work is intended architecture, not merely a speculative
Wi-Fi alternative. No implementation dates or exact wake timings are chosen.

### V1 cabin installation and permanent manual SYNC

Plan the first installed version with the logger in the glove box/cabin and
wiring from the designated under-hood charging/jump points into the cabin.
V1 does not require IBS/LIN access or a separate ignition/switched-12V wire for
vehicle-on detection. Route/circuit/connector details and installed validation
remain work to do; the enclosure is not planned for the engine bay.

Implement physical manual SYNC/wake and a BLE advertising/connection window.
It remains permanently available as a backup and explicit user-access path.
The first BLE version may use a reset/manual-button-initiated awake window;
the exact button/wake circuit and window timing are not chosen. Do not assume
a phone can remotely wake a fully sleeping radio.

### BMW charging-point topology — OPEN, research before any trunk-side decision

The planning question arises from BMW owner charging instructions commonly
directing chargers to designated under-hood points rather than the trunk
battery. No authoritative BMW evidence resolving the user's installation was
found in this repository. That premise is a research lead, not an established
explanation of the sensing topology.

When the logger moves to the trunk for IBS/LIN access, can its power/solar/
measurement connections correctly be made near the battery, or must they still
connect electrically through the designated under-hood points? **Unanswered.**
Find authoritative BMW documentation applicable to the **2017 M240i / F22** and
record evidence for each of these questions:

- Why does BMW specify the under-hood charging/jump points?
- Is the purpose to route charging current through, or make it observable to,
  the IBS/energy-management system?
- Would direct battery-positive and/or battery-negative connections bypass a
  required sensing path? Do the positive and negative rules differ?
- Is there a correct trunk-side connection point preserving IBS measurement
  and BMW energy-management behavior?
- Does guidance for permanent low-current accessories/loggers differ from
  guidance for external battery charging?
- Which findings apply to this exact vehicle/platform, rather than another
  BMW model, model year or electrical configuration?

Until this research is complete: **V1 = under-hood charging points; future
trunk wiring topology = OPEN.** The expected trunk enclosure location is not a
wiring decision. This documentation stint does not answer these questions.

### F22 IBS/LIN access and future vehicle-on wake — OPEN

During the later IBS phase, expect to move the logger near the trunk battery/
IBS wiring. Research exact F22 wiring, connector/access locations and available
signals. Do not assert a convenient IBS/LIN tap exists in the cabin. Determine
whether IBS/LIN or another signal found during that integration can supply
vehicle-on/off detection; a separate switched ignition wire is not assumed.
The signal, electrical interface, detection criteria and vehicle-off behavior
remain unchosen. Manual physical SYNC must continue to work independently.

A possible **future vehicle-active mode** could stay awake, sample more often,
capture richer charging/alternator telemetry and provide BLE, then return to
parked/deep-sleep behavior after vehicle-off. This is an idea, not implemented
behavior or a chosen cadence/state-transition design.

### Native iPhone BLE app and durable record sync — PLANNED, not implemented

Build BLE discovery/connection, requests for unsynced records, durable local
phone storage, and ACK only after successful durable save. Add display, graphs
and configuration. Define the BLE protocol/profile, interrupted-sync recovery
and local persistence behavior; preserve sequence-based ordering and the
proposed cumulative ACK semantics. Current USB command ACKs are not storage
ACKs. Firmware sync/ACK/reclamation remain unbuilt; storage-full policy and its
interaction with sequence authority still need their own decision.

### Phone wall-clock synchronization — PLANNED, not implemented

The phone is the intended clock source, supplying epoch/time mapping over BLE.
Define the D-019 operation and clock-age/mapping behavior without making
correctness depend on Wi-Fi/NTP. Preserve existing UNKNOWN-time / epoch-0
records and distinguish any host-derived timestamp from what the device knew
at capture. Clock drift and deep-sleep wall-clock retention still need the
measurements listed in STORAGE_SYNC_DESIGN. USB time-setting may serve bench
work; it is no longer the first installed clock dependency.

### Self-hosted server integration — PLANNED alongside the native iOS app

The phone uploads using whatever normal Internet connection it has. The server
owns long-term storage, web UI/historical access, analysis, inference and
higher-level processing. Design upload/retry, deduplication and phone retention
across interrupted Internet access. ACK to the ESP follows durable phone save,
not server availability; phone ACK is not proof of server receipt. The ESP must
not know about or depend on the home inference/web server. Home Wi-Fi proximity
is never assumed; direct logger Wi-Fi is optional later work.

## Firmware modularization — FOUR STAGES BUILT, LATEST 2026-09-23

`Arduino/solar-logger/solar-logger.ino` is 7,059 lines by `wc -l` after the
telemetry extraction on 2026-09-23. It was 7,201 after the NVS extraction
earlier the same day, 7,836 after the connection extraction on
2026-09-18, 7,949 after the INA228 extraction the same day, 9,126 before that,
and 8,436 when this plan was written. The
agreed direction is
real `.h`/`.cpp` modules, **not** additional Arduino `.ino` tabs: extra tabs are
concatenated into the same translation unit, so they hide nothing, enforce
nothing, and keep the file that a person edits from being valid C++ on its own
(D-039).

**Five modules exist**, each extracted with no intended behavior change:

- **The INA228 driver**, `ina228.h` and `ina228.cpp`, 2026-09-18. A hardware
  smoke test of that image was reported later the same day; it is recorded, as
  reported, in [LAB_NOTES.md](LAB_NOTES.md). The boundary rules it set are
  [DECISIONS.md](DECISIONS.md) D-047.
- **Connection bookkeeping**, `connection.h` and `connection.cpp`, 2026-09-18:
  which transport a host has claimed. Compiled and host-tested; the supplied
  `8776ba0` hardware transcript confirms claim/status/keepalive/release. Its
  boundary is D-049.
- **NVS persistence**, `nvs_persistence.h` and `nvs_persistence.cpp`,
  2026-09-23: the `Preferences` object, the namespace, every key, and the typed
  reads and writes. Compiled and host-tested; restore and checkpoint-write
  smoke tests passed on hardware revision `d017f7d`, preserving Experiment 3.
  See the 2026-09-23 LAB_NOTES addendum for limits. Its boundary is D-050.
  **NVS only** moved; RTC-retained state remains a separate future boundary,
  not part of NVS or a combined storage/scheduler module (D-055).

- **Telemetry**, `telemetry.h` and `telemetry.cpp`, 2026-09-23: how each
  machine-readable CSV line is spelled, and nothing about when one is emitted.
  Compiled and host-tested; it has **not** run on hardware. Its boundary is
  D-051. This is the `telemetry` row of the module table below, built as that
  row describes.
- **Record format**, `record_format.h` and `record_format.cpp`, 2026-09-24: the
  deployed 72-byte version-1 record, its flag bit values and sentinels, and its
  CRC and structural validation. Compiled and host-tested, with
  `record_format.cpp` itself compiled on the host and run against the real
  Experiment 3 record; it has **not** run on hardware. Its boundary is D-056.
  This is the `Record format / CRC` row of the module table below, built as that
  row describes. Record version is still 1 and no byte of the layout changed.

Telemetry was the fourth stage chronologically and is numbered 5 in the
original table below, because that table split the INA228 work into two rows.
Those plan numbers are not the chronological stage numbers. Record format was
the fifth and has no number in that table at all: it is the first of the
boundaries D-055 separated out of the old `auto_state` proposal.

The audits and hardware checks are in [LAB_NOTES.md](LAB_NOTES.md). The gate
every stage passes was built on 2026-09-18, and since the connection
stage it also regenerates the editor database, so a new module needs no editor
configuration (D-048).

### What the source actually contains

Measured on 2026-09-16, not estimated:

| | |
| --- | ---: |
| Lines | 8,436 |
| Top-level functions | 119 |
| Mutable file-scope globals | 38 |
| `RTC_DATA_ATTR` globals | 12 |
| `Preferences` objects | 1 |

### State ownership inventory — added 2026-09-17

Read out of the source, not inferred. The point of the table is the last column:
where two things may write one variable, the module boundary has to put them on
the same side of the line or the split freezes the ambiguity in place.

Persistence classes: **V** volatile RAM (lost on any reset), **R** RTC-retained
(survives deep sleep, lost on power loss), **N** NVS (survives power loss),
**L** LittleFS.

| State | Class | Written by | Invariant |
| --- | :---: | --- | --- |
| `experimentId` | N | `loadCheckpoint()`, `resetExperiment()` | Monotonic, never decreases. The autonomous path reads it via `loadExperimentIdOnly()` and **must never write it** (D-024). |
| `completedInterval`, `runningCharge_mAh`, `runningEnergy_mWh` | N | `loadCheckpoint()`, `closeMeasurementInterval()`, `resetExperiment()` | Advance only through a successful interval close. The autonomous path never touches them (D-017 accounting isolation). |
| `intervalStartMs` | V | `setup()`, `resetExperiment()`, `closeMeasurementInterval()`, `hostSessionHold()`, `resumeIntervalAccounting()` | **Five writers.** Must always match the moment the accumulators were last reset; today `setup()` and `hostSessionHold()` disagree by tens of ms on the claimed-rendezvous path. |
| `autonomousTestArmed` | N + V mirror | `loadAutonomousSettings()`, `armAutonomousTest()`, `stopAutonomousTest()`, `autonomousDeepSleepAgain()` | RAM must follow NVS. A failed NVS write must leave both alone; two paths currently clear only RAM. |
| `autonomousTestRunning` | V | arm, stop, wake cycle, host hold/release, RTC-loss path | Means "the autonomous cycle owns the board this awake period". Not persisted, correctly. |
| `autonomousIntervalSeconds` | N | `loadAutonomousSettings()`, `saveAutonomousInterval()` | The single cadence authority (D-018). Clamped 10–3600 on both load and set. Refused while armed. |
| `autonomousState` | V | `setAutoState()` only | Single writer, prints every transition. The one piece of state in the file with a clean owner. |
| `rtcAutoMagic` | R | wake path, arm, host handoff, `stopAutonomousTest()` | Guards the other nine `rtcAuto*` values. Must be set **after** they are all valid, never before. |
| `rtcAutoBootId` | R | `nextBootId()` callers | One continuous power-on session. Increments on cold boot; a timer wake keeps it. Backed by NVS `boot_id`. |
| `rtcAutoSessionElapsedMs` | R | deep-sleep scheduler, host handoff, arm, `setup()` (cold-boot resume, RTC-loss rebuild). Corrected 2026-09-18: the wake cycle reads it and never writes it. | **Monotonic within `boot_id`** (D-033). Pre-advanced by the commanded sleep before sleeping, so it is a commanded clock, not a measured one. **Exact at one instant**: the scheduler sets it for the next wake's `setup()` entry, the handoff for the handoff itself, and "now" adds only the time since that instant (D-046). |
| `rtcAutoIntervalStartMs` | R | wake cycle (after reset), scheduler overrun branch, host handoff, arm, `setup()` (cold-boot resume, RTC-loss rebuild) | The autonomous interval boundary. Must move only when the accumulators are reset. **The overrun branch breaks this**; found 2026-09-18, not fixed, below. |
| `rtcAutoNextSeq` | R | `autoStorageRecover()`, wake cycle on successful append | Never reused (D-017). Advances only after a durable append. |
| `rtcAutoSeqHighWater` | R, mirrors N `auto_seq_hw` | `reserveSequenceBlock()`, `autoStorageRecover()` | The floor that makes duplicates impossible. A failed NVS read currently lowers it to 0. |
| `rtcAutoRunChargeUAh`, `rtcAutoRunEnergyUWh` | R, rebuilt from L | wake cycle, `autoStorageRecover()`, `clearStorage()` | Autonomous-local totals, **not** experiment totals. Since the 2026-09-17 fix, advanced only after a successful durable append. |
| `rtcAutoCycleCount` | R | wake cycle, arm, cold-boot resume, RTC-loss path | Zero means "first record of this session", which is what sets `FIRST_AFTER_BOOT`. |
| `rtcAutoCommandedSleepMs` | R | `autonomousDeepSleepAgain()` | **Written, never read.** No invariant, because nothing depends on it. |
| `rtcSleepTestMagic`, `rtcSleepTestCycle` | R | `armSleepPowerTest()`, `enterSleepPowerTestDeepSleep()`, `stopAllPowerTests()`, cold-boot resume | Belong to power-test state, not RTC-retained autonomous state. Guarded by their own magic (D-011). |
| `activeTransports` | V | `connectionClaim()`, `connectionRelease()` | A bit is set only by an explicit software lease, never by electrical presence (D-025). Moved into `connection.cpp` as a `static` on 2026-09-18; nothing outside reads it (D-049). |
| `hostSessionHeld`, `hostLeaseDeadlineMs`, `hostLeaseRenewedMs`, `hostKeepaliveCount`, `hostSessionStartedMs`, `hostSessionIntervalCloses` | V | `hostSessionHold()`, `hostSessionKeepalive()`, `hostSessionRelease()`, `serviceHostLease()`, `stopAutonomousTest()` | A lease is a property of one awake period, so plain RAM is right. `hostSessionIntervalCloses` is incremented inside `closeMeasurementInterval()` — measurement writing session state. |
| `hostSessionBaselineEstablished` | V | `hostSessionHold()`, release, lease expiry, `stopAutonomousTest()` | Means "the accumulators were reset at the claim". `setup()` reads it to decide whether to reset again. A cross-function ordering contract with no enforcement. |
| `hostClaimedRendezvous` | V | `hostSessionHold()`, rendezvous loop, release, expiry, stop | The rendezvous loop's exit condition. Set by the command parser, read by the autonomous orchestrator — the coupling that the `pumpCommands` injection is meant to formalize. |
| `pendingAutonomousSleep`, `pendingAutonomousSleepDeadlineMs`, `pendingAutonomousSleepPath` | V | `armPendingAutonomousSleep()`, `cancelPendingAutonomousSleep()`, `servicePendingAutonomousSleep()` | The deferred-sleep grace (D-035, D-044). Bounded, and must stay bounded. Row corrected 2026-09-18: it named `releaseSleepPending` and `servicePendingReleaseSleep()`, which do not exist in the current source. |
| `hostHandoffAtMs` | V | `beginAutonomousSleepFromHostSession()` only | Added 2026-09-18 (D-046). The `millis()` instant at which the handoff set `rtcAutoSessionElapsedMs`; the scheduler's `AUTO_SLEEP_HOST_RELEASE` path adds only the time since it. |
| `intervalAccountingBlocked` | V | `resumeIntervalAccounting()` | Deliberately not persisted: `setup()` clears accumulators on every boot, so a reset is the cure (D-012). |
| `inaShutdownActive` | V | `enterSleepPowerTestDeepSleep()` after a verified `enterInaShutdownMode()`, `stopAllPowerTests()`, cold-boot resume in `setup()`. Corrected 2026-09-18: this row named `enterInaShutdownMode()`, which never writes it. | While set, nothing may print INA registers as measurements (D-016). Read by STATUS and the heartbeat. The driver neither reads nor writes it, so it stayed in the sketch when the driver moved (D-047). |
| `wifiPowerTest*`, `sleepPowerTest*` | N flags + V runtime | arm/stop/resume paths | One armed flag plus one variant flag makes "both variants armed" unrepresentable (D-013, D-014). |
| `autonomousStopRequested` | V | `stopAutonomousTest()` | Set when a stop is *asked for*, before it is known to have worked. Plain RAM on purpose. |
| `bootWakeupCause`, `setupEntryMicros` | V | first two lines of `setup()` | Write-once. Everything that reports a wake timing reads `setupEntryMicros`. |
| durable log `/auto.bin` | L | `autoStorageAppend()` only | Append-only. Nothing else writes a record, which is what lets an intact tail be the sequence authority (D-023) — and what stops being true once pruning exists. |

Three ownership problems fall straight out of the table and are written up under
"Known bugs and issues":

- **`intervalStartMs` has five writers** and no single rule tying it to the
  accumulator reset it is supposed to timestamp.
- **The `armed` flag exists twice** — NVS and a RAM mirror — with no single
  function responsible for keeping them equal.
- **Resolved 2026-09-17:** `rtcAutoRunChargeUAh` previously advanced before
  storage committed its record. Both retained totals now advance only after a
  successful append; the source characterization tests pin that ordering.

### Module boundaries — revised plan, 2026-09-23

D-055 replaces the old broad `auto_state` proposal. The historical analysis
below remains useful for the couplings it found, but grouping record format,
files, sequence authority, RTC variables and scheduling in one module is no
longer the plan. Future rows below describe responsibilities, **not chosen
filenames or completed extractions**.

| Module or future concept | Owns | Boundary / status |
| --- | --- | --- |
| `nvs_persistence` | Preferences namespace, keys and typed reads/writes | Extracted; caller decides when to change values; no RTC ownership (D-050) |
| `connection` | Transport claims and queries | Extracted; does not own leases, BLE radio behavior or sleep policy (D-049) |
| `ina228` | Register access, device configuration and sensor/accumulator operations | Extracted; callers own measurement timing and accounting (D-047) |
| `telemetry` | CSV formatting/emission mechanics | Extracted and host-tested; hardware validation pending (D-051) |
| Record format / CRC | Version-1 layout, field/flag constants, CRC and record validation | Extracted 2026-09-24 as `record_format`, exactly as this row describes; the Experiment 3 golden bytes are preserved and now executed against the module's own source; no filesystem, RTC, sequence allocation or scheduler moved (D-056). Host-tested, **not hardware validated** |
| LittleFS storage mechanics | Mount, record I/O, append, scan/truncate mechanics and explicit recovery/error results | Planned; use the record contract; expose log facts without deciding sequence authority, reclamation or corruption policy during a move |
| Sequence authority / reservation policy | D-023 reconciliation of intact log tail versus NVS reservation, allocation and persistence ordering | Separate concept; consumes storage results and NVS access; RTC holds retained values, not this decision policy. Future placement/API not chosen |
| RTC-retained autonomous state | Retained autonomous values, validity and lifetime across deep sleep | Planned; no file I/O, record encoding or scheduling; keep power-test RTC state separate |
| Autonomous scheduling / policy | Wake-cycle ordering, interval deadlines, runtime state transitions/ownership queries, rendezvous and sleep/handoff decisions | Planned; consumes explicit sensor, storage, sequence, retained-state and connection results; no new behavior hidden in extraction |
| Experiment accounting | Tethered interval/experiment state, interval close and checkpoint timing | Planned; calls existing INA/NVS/telemetry interfaces and explicit autonomous-ownership queries |
| Power tests | Wi-Fi/deep-sleep test state and their separate RTC lifetime | Planned; keep separate from autonomous retained-state ownership |
| Session logic | Lease acquisition/renewal/release, expiry and accounting handoff | Planned; transport claims stay in `connection`; scheduling coordination is an explicit interface |
| Command protocol / composition | Parse and dispatch, bounded ACK/RESULT writer, boot/loop ordering and command-pump wiring | Planned later; command protocol stays distinct from CSV and future storage ACKs |

The `record_format` extraction was stopped before it began when the DIAG_ALRT
correctness issue was confirmed, and was then performed on 2026-09-24 as a
structural move with that correctness change already in source. The reorder
itself is source-tested and remains **not uploaded or hardware validated**;
the extraction did not touch it and the tests that pin `DIAG_ALRT` before
CHARGE and ENERGY are unchanged and still green. Record version is still 1. The
golden record establishes 72 packed bytes, coverage 0–67 and CRC at 68; it does
not validate overflow behavior, and the extraction does not either.

The old `ina228_regs` / `ina228` rows were deliberately built as one module in
September 2026. No separate register module is required by this revised plan.
The dependency direction is from scheduling/composition toward mechanics and
explicit state/results. The revised call graph must be checked when interfaces
are scoped; the historical counts below do not prove the new graph is acyclic.

### Dependency direction — historical analysis, corrected 2026-09-16

**Historical graph, not the current module grouping.** The `auto_state` and
`nvs_rtc` labels through the following graph discussion describe the old
proposal. D-050 and D-055 supersede those groupings; the revised table above is
the current plan. No fresh graph measurement is claimed by this doc update.

An earlier version of this plan claimed "exactly one hard cycle, `command` ↔
`autonomous`." **That was wrong.** A full call-graph pass over all 119
functions, with comments and string literals stripped, found **three** mutual
pairs, and all three run through autonomous scheduling:

| Boundary | down | up |
| --- | ---: | ---: |
| `commands` ↔ `autonomous scheduling` | 6 | 2 |
| `host sessions` ↔ `autonomous scheduling` | 8 | 8 |
| `measurement/accounting` ↔ `autonomous scheduling` | 3 | 8 |

### Historical graph: why autonomous looked bidirectional

Because **autonomous scheduling is not a layer.** It is two things sharing one
name, and they belong at opposite ends of the stack:

- **State and storage** — `AutoState`, the ten `rtcAuto*` RTC globals, `setAutoState()`,
  `autonomousOwnsBoard()`, the record encode/CRC/append path. This half is
  called *by* everything and calls almost nothing. It belongs near the bottom.
- **Orchestration** — `runAutonomousWakeCycle()`, `autonomousUsbRendezvous()`,
  `autonomousDeepSleepAgain()`, `beginAutonomousSleepFromHostSession()`,
  `autonomousColdBootMaintenanceWindow()`. This half drives measurement,
  sessions and the command pump. It belongs at the top, beside `setup()` and
  `loop()`.

Draw those as one box and every edge through it reads as `↕`. Split them and
the arrows point one way.

A second split is needed for the same reason, and the original plan already had
it: `connection` (the transport bitmask, `anyHostConnected()`, claim/release)
must be separate from `host_session` (the lease itself). D-025 requires the
autonomous sleep policy to *read* connection state; D-034 requires the session
to *invoke* autonomous scheduling. Those point opposite ways only while both
live in one module.

### Historical graph: measured effect of the old splits

With `auto_state` / `auto_orchestrator` and `connection` / `host_session`
separated, the module graph has **two** remaining cycles, and both pass through
the same single edge:

```text
auto_orchestrator -> commands -> auto_orchestrator
auto_orchestrator -> commands -> host_session -> auto_orchestrator
```

That edge is `autonomousUsbRendezvous()` (line 6177) and
`autonomousColdBootMaintenanceWindow()` (line 6636) calling
`handleSerialCommands()`. It exists because D-021 requires a guaranteed
management window, which means the autonomous path has to pump the parser.

**Breaking that one edge makes the whole module graph acyclic.** Verified by
re-running cycle detection with it removed: 0 cycles. The autonomous module
takes a `void (*pumpCommands)()` supplied by `solar-logger.ino`, the only place
allowed to know about both. Two call sites.

### Historical dependency stack — superseded grouping

Computed from the real call graph after both splits and the injection. Higher
depends on lower; nothing points upward.

```text
7  solar-logger.ino     setup(), loop()
6  commands             parser, CMD_ACK/CMD_RESULT, bounded writer
5  host_session         the lease: HOLD / KEEPALIVE / RELEASE, serviceHostLease
4  auto_orchestrator    wake cycle, rendezvous, deadline scheduler, arm/stop
3  power_test           Wi-Fi and both deep-sleep variants
2  measurement          INA228 driver, sampling, interval accounting, CSV
1  auto_state           AutoState, RTC globals, record encode/CRC/append
0  connection           transport bitmask                      (leaf)
0  nvs_rtc              Preferences, every typed get/put       (leaf)
```

Note where the two halves land: `auto_orchestrator` at 4, **above**
measurement; `auto_state` at 1, **below** it. A single "autonomous scheduling"
box has to span levels 1 through 4, which is exactly why it could not be
stacked.

`connection` and `nvs_rtc` are the only true leaves — zero outgoing
cross-module calls each.

### Historical call-site correction — 2026-09-18

This section used to list two, both in the `measurement` → `auto_orchestrator`
direction. **Only one is real:**

- `intervalAccountingSuspended()` and `intervalAccountingSuspendReason()` call
  `autonomousOwnsBoard()`. The old plan placed that query in `auto_state`.
  D-055 now calls for an explicit ownership-query boundary; this historical
  placement is not an instruction to recreate the combined module.

The second entry said `validateInaForWake()` calls `autonomousDeepSleepAgain()`.
**It does not.** Re-read on 2026-09-18: its body only reads registers and
returns, and the scheduler's call sites are exactly `runAutonomousWakeCycle()`,
`beginAutonomousSleepFromHostSession()`, `servicePendingAutonomousSleep()`, and
`setup()` twice. `test_characterization_policy.py` now pins that set. Likely
cause, not verified: the forward declaration
`void autonomousDeepSleepAgain(AutoSleepPath path);` sits immediately after
`validateInaForWake()` ends, and a call-graph pass that did not tell a
declaration from a call would attribute it there. `validateInaForWake()` still
belongs in `auto_orchestrator`, because only the wake cycle calls it, but it
carries no sleep authority to remove.

### The eleven execution paths — added 2026-09-17

Traced from source. This is the list each extraction stage's hardware check has
to be chosen against: a stage is safe when every path that crosses the moved
boundary still has somewhere to be exercised.

`RESET` in the sleep column means a deep sleep, which on this chip is a reboot.

| # | Path | Entry | Persistence touched | INA228 accumulators | Ends in |
| ---: | --- | --- | --- | --- | --- |
| 1 | Cold boot, not armed | `setup()` full init | NVS read (checkpoint, all test flags) | **reset**, interval starts | `loop()` |
| 2 | Cold boot, armed | `setup()` → maintenance window → resume | NVS read + `boot_id` write + reservation; LittleFS scan | **reset** after the window, first interval one full cadence | `RESET` via `AUTO_SLEEP_COLD_BOOT` |
| 3 | Timer wake, unclaimed | early branch → `runAutonomousWakeCycle()` → rendezvous | LittleFS **append**; NVS reservation only when the block is exhausted | **read, then reset** — the ordering D-020 exists for | `RESET` via `AUTO_SLEEP_TIMER_WAKE` |
| 4 | Timer wake, RTC state lost | early branch → rebuild | `boot_id` write, LittleFS scan, reservation | **reset**, interval discarded and said so | `RESET` via `AUTO_SLEEP_RTC_LOST` |
| 5 | HOLD during a rendezvous | rendezvous pumps parser → `hostSessionHold()` | none | **reset** at the claim (D-028) | falls through into `setup()` init, then `loop()` |
| 6 | Held host session | `loop()` | NVS checkpoint every closed interval | closed and reset once per tethered interval | stays awake |
| 7 | KEEPALIVE | `hostSessionKeepalive()` | none | none | lease extended |
| 8 | RELEASE | `hostSessionRelease()` → `beginAutonomousSleepFromHostSession(sleepNow=false)` | NVS checkpoint via the interval close | **closed and reset** at the handoff | `RESET` after the 250 ms grace (D-035) |
| 9 | Lease expiry | `loop()` → `serviceHostLease()` → same handoff, `sleepNow=true` | same | same | `RESET` immediately, no grace |
| 10 | `AUTONOMOUS OFF` | `stopAutonomousTest()` | NVS armed flag cleared | none | stays awake; a held session is deliberately kept (D-031) |
| 11 | Upload takeover | `upload.sh` → best-effort HOLD → esptool | none on the device | none | esptool resets the board; the lease ceases to exist |

Three things the table makes visible:

- **Paths 3 and 5 share a boundary that nothing enforces.** Path 5 begins inside
  path 3's rendezvous loop and then continues into `setup()`'s ordinary
  initialization, which was written for path 1. The only thing stopping it from
  resetting the accumulators a second time is the
  `hostSessionBaselineEstablished` flag, and it does not stop `setup()` from
  rebasing `intervalStartMs` or from rewriting `ADC_CONFIG`.
- **Paths 8 and 9 are the same accounting transition with different acknowledgement
  needs**, which is exactly why they were unified behind
  `beginAutonomousSleepFromHostSession()`. That unification is the part of the
  design most worth preserving verbatim through stage 10.
- **`armAutonomousTest()` is a twelfth path that is not in this table**, because
  it sleeps directly rather than through any of the transitions above. That is
  the defect written up under "MUST FIX BEFORE MODULARIZATION". **Resolved
  2026-09-17:** it now arms, returns its result, and sleeps from `loop()` after
  the 250 ms grace, through the same deferred path RELEASE uses. That shared
  grace window is where the second 2026-09-18 finding below lives.

### Extraction history and remaining boundaries

Keep each extraction narrow and validate its dependencies before moving it.
The numbered table below records the original plan and the stages already
built; it is not a schedule for one combined `auto_state` extraction. Hardware
smoke tests remain deliberate follow-up work, not automatic uploads.

| Stage | Move | Why it is safe | Hardware check afterwards |
| ---: | --- | --- | --- |
| 1 | `nvs_rtc` | true leaf — zero outgoing cross-module calls. **BUILT 2026-09-23** as `nvs_persistence`, NVS only | reboot; `experiment_id` and interval number survive. **OBSERVED on d017f7d** |
| 2 | `connection` | the other true leaf; 1 global, 5 functions. **BUILT 2026-09-18**, as planned: exactly that global and those five functions | `LOGGER SESSION STATUS` reports the same transports. **OBSERVED on 8776ba0** |
| 3 | `ina228_regs` | leaf but for the bus; no globals | `STATUS` reports live V/I/P/temperature |
| 4 | `ina228` | above stage 3; no globals, since `inaShutdownActive` stayed in the sketch (D-047) | `POWER TEST STATUS` reports the real INA mode; heartbeat sane |
| 5 | `telemetry` | formatting only, no state. **BUILT 2026-09-23**, as planned: no state moved, and globals are unchanged at 36,332 bytes | one `CSV_SAMPLE` and one `CSV_DATA` row reach the logger unchanged. **PENDING** |

**Stages 3 and 4 are BUILT, 2026-09-18, as one module, `ina228`, ahead of
stages 1 and 2.** A hardware smoke test covering their checks was reported
later the same day and is recorded, as reported, in
[LAB_NOTES.md](LAB_NOTES.md). Two departures from this table, both at the
direction of that stint's brief and recorded here so neither is silent:

- **Order.** The driver calls nothing but `Wire`, `Serial` and `delay()`, so it
  was a leaf in the same sense as stages 1 and 2. "Nothing moves before the
  things it calls" still held.
- **Batching.** The two stages were done as one, against "Do not batch them."
  The hardware check afterwards is both rows' checks together. The full list is
  in [LAB_NOTES.md](LAB_NOTES.md), under the 2026-09-18 INA228 entry.

**Plan stage 2 is BUILT, 2026-09-18, and its hardware smoke is recorded in the 2026-09-23 addendum.** It moved
exactly what the row above says. One departure from the module table is
recorded in D-049: `printUsbPresence()` stayed in the sketch.

**Plan stage 1 is BUILT, 2026-09-23, and its NVS restore/write smoke passed on `d017f7d`.** One
departure from the module table, recorded in D-050: the row's RTC half did not
move, so the module is `nvs_persistence` rather than `nvs_rtc`. Two functions
were split instead of moved whole, `loadAutonomousSettings()` and
`reserveSequenceBlock()`, each because it mixed a typed NVS access with a
decision the caller owns. Its hardware check is the sharpest of the three so
far: the board must come back as Experiment 3 with the same interval number and
running totals.

**Plan stage 5 is BUILT, 2026-09-23, and its hardware check is PENDING.** It
moved exactly what the row above says, with one addition recorded in D-051:
`CSV_HEADER` moved with `CSV_DATA` because it names that row's fields. One
function was split rather than moved whole, `printLiveSample()`, because it
mixed measurement policy and acquisition with the row construction; only the
construction moved. Four of the five moved bodies are token-identical to
`HEAD`, and the fifth differs only where loose parameters and three sketch
globals became one explicit struct.

### Remaining extraction plan — concepts, not filenames

The DIAG_ALRT change still owes its own hardware validation of the overflow path;
no hardware result is claimed here. Record format/CRC is **BUILT** (D-056); the
version-1 byte/CRC contract and the golden record are unchanged, and no behavior
fix was folded into the move. LittleFS storage mechanics is the next scoped
candidate, and it has to carry the known corrupt-record/tail defect as an
explicit correctness change rather than absorbing it into a move.

| Future boundary | What must remain separate | Validation focus when undertaken |
| --- | --- | --- |
| ~~Record format / CRC~~ **BUILT 2026-09-24** | Nothing else moved: no LittleFS, RTC, sequence allocation or scheduling | Done as stated: exact 72-byte golden record, CRC coverage 0–67, CRC offset 68, unchanged version 1 and validation semantics, now also asserted per field at compile time |
| LittleFS mechanics | No sequence-authority or storage-full policy decision | Preserve append/read/recovery behavior; handle the known corrupt-record/tail issue as an explicit correctness change, not silent cleanup |
| Sequence authority / reservation | Not merely a side effect of storage scan or retained-state access | D-023 intact/partial/corrupt/empty cases, NVS write failure and duplicate avoidance; reclamation must revisit authority explicitly |
| RTC-retained autonomous state | No record encoding, filesystem or scheduler | Single definitions and retained lifetime/validity, session clock continuity and separate power-test state |
| Autonomous scheduling / policy | Consume mechanics/state through explicit boundaries | Existing deadline, handoff and command-pump contracts; preserve the known overrun xfail until its own fix |
| Experiment, power-test, session and command concerns | Retain accounting ownership and the separation of transport claims from leases | Scoped characterization, required local gate and later deliberate hardware checks for each move |

These rows are separate scopes, not permission to batch them. Exact filenames,
interfaces and order after record format remain to be chosen from actual
coupling. Do not carry forward the old numeric stage 6 as an all-in-one module.
RTC lifetime and command-pump inversion remain high-risk boundaries; moving
normal timer-wake or session logic still requires its own hardware acceptance,
including the cold-boot maintenance window and lease expiry where applicable.

### The gate every stage passes — BUILT 2026-09-18

Each stage is one commit, and a stage is not done until all of this holds, in
order:

1. `tools/check.sh` reports `ALL OK` **before** the move, so a failure
   afterwards belongs to the move.
2. The move is made. No behavior change rides along with it.
3. `tools/check.sh` reports `ALL OK` again. The characterization tests are
   unchanged. A test edited to fit the move is a behavior change and needs a
   separate reason.
4. The editor database covers every new file. Since 2026-09-18 the
   `intellisense` step of `tools/check.sh` regenerates and verifies it, so
   step 3 already did this and there is nothing to configure (D-048).
5. `tools/upload.sh`, deliberately, then the smoke test below, then that
   stage's own hardware check from the table above.

See [DECISIONS.md](DECISIONS.md) D-043.

#### Hardware smoke test after an extraction group

Short enough to run after every upload. Commands are sent one at a time, and
each must exit 0.

```text
tools/send.sh VERSION
tools/send.sh LOGGER STORAGE INFO
tools/send.sh LOGGER AUTONOMOUS STATUS
tools/send.sh --wait LOGGER SESSION HOLD
tools/send.sh LOGGER SESSION KEEPALIVE
tools/send.sh LOGGER SESSION RELEASE
```

Waiting rules that matter here (D-040). The first three wait for the next
rendezvous by default. HOLD may wait; `--wait` is accepted for it and is
already its default. KEEPALIVE and RELEASE never wait, and an explicit
`--wait` on either is refused with exit 2. A one-shot HOLD is not renewed by
anything, so KEEPALIVE has to follow within the firmware's 15-second lease and
RELEASE after it.

Expected after each future extraction. These outcomes have been observed on
modularized images, including `8776ba0` and `d017f7d`; see the latest LAB_NOTES
addendum. A past pass does not validate the next image:

- `VERSION` prints the expected version, a real revision hash rather than
  `UNKNOWN`, and `solar-logger-protocol-ack-v3`.
- `LOGGER STORAGE INFO` reports at least the record count seen before the
  upload, with `Tail status: INTACT`. Experiment 3's log must survive every
  stage.
- `LOGGER AUTONOMOUS STATUS` reports the same armed state as before the upload.
- HOLD, KEEPALIVE and RELEASE each show `CMD_ACK` and `CMD_RESULT,...,OK`.
  RELEASE ends with the capture reporting that the transport disappeared after
  the successful result.

**Extractions touching retained state, experiment accounting, autonomous
policy or host sessions need more than this.** The old stage numbers no longer
define those boundaries. Run the full
acceptance sequence in [LAB_NOTES.md](LAB_NOTES.md) 2026-09-17 after each of
them, including lease expiry (step 7), five consecutive unclaimed records
(step 8), and the refusals (steps 9 and 10).

### Arduino-specific concerns

- **`RTC_DATA_ATTR` definitions must live in exactly one `.cpp`** and be
  declared `extern` in the header. Defining one in a header would place a
  separate copy in each translation unit, and the autonomous session clock
  would silently reset. This is the single highest-risk detail in the whole
  plan.
- **There are twelve `RTC_DATA_ATTR` globals, not eleven, and they split across
  two ownership domains.** Ten are `rtcAuto*` and belong to retained autonomous
  state; `rtcSleepTestMagic` and `rtcSleepTestCycle` belong to power-test state
  (D-011). Counted by `grep -c RTC_DATA_ATTR` on 2026-09-17. Moving all twelve
  under one autonomous validity guard would put the deep-sleep power test's cycle counter under the
  autonomous module's magic guard, where a `stopAutonomousTest()` that clears
  `rtcAutoMagic` sits next to state it does not own.
- **`rtcAutoCommandedSleepMs` now has a consumer.** RESOLVED 2026-09-17. It was
  assigned once and referenced nowhere else. It was kept rather than deleted,
  because it was an incomplete feature rather than an abandoned one: the wake
  report now prints it next to the interval duration, which makes visible the
  thing that was otherwise invisible — `interval_ms` is a COMMANDED sleep plus
  measured awake time, not measured elapsed time. See "The firmware cannot
  measure its own RTC drift" below for what it still does not answer.
- **`setup()`'s early timer-wake branch must keep running before any
  initialization** (D-020, D-024). Modularization must not turn it into a
  constructor-ordering problem; static objects with non-trivial constructors do
  not belong in these modules.
- **Arduino CLI stops synthesizing prototypes** for code that moves out of the
  `.ino`. That is a gain, not a loss, and the forward-declaration block in the
  sketch shrinks by one entry per function moved. The INA228 stage removed no
  entry: each of its functions was defined above its first caller, so none was
  in the block. The connection stage removed two, `anyHostConnected()` and
  `printActiveTransports()`, leaving seventeen.
- **A `.cpp` does not get the `#include <Arduino.h>` that Arduino CLI prepends
  to the sketch.** It includes `<Arduino.h>` and any library it uses itself
  (D-047).
- **A header included at the top of the sketch is above the generated
  prototypes**, so a type it defines can appear in them. `struct SensorReading`
  used to sit in the sketch at a position a comment guarded. That comment was
  corrected in the INA228 stage rather than left describing a hazard that no
  longer exists.
- `.cpp` files in the sketch directory are compiled automatically; no build
  file changes are needed. Confirmed by the INA228 stage.
- `tools/intellisense.sh` fingerprints every source file in the sketch
  directory, `.h` and `.cpp` included. **CORRECTED 2026-09-18:** this line said
  `.cpp` modules also get correct database entries for free. They do not. The
  database's entry for `ina228.cpp` names the build copy,
  `build/intellisense/sketch/ina228.cpp`, and `tools/intellisense.py` adds an
  editor entry only for the `.ino`. Predicted, not observed in the editor:
  cpptools applies no ESP32 include paths to `ina228.cpp`. The real build is
  unaffected. Direction: have the script add and verify an entry for each
  sketch `.cpp`, the way it does for the `.ino`. Not done in that stint.
  **RESOLVED 2026-09-18 (D-048)**, generically: every C and C++ file Arduino
  CLI compiles for the sketch gets one verified entry naming the real file,
  found by discovery, and `tools/check.sh` regenerates the database.
  `connection.cpp` was configured by it with no edit. What VS Code displays
  has still not been observed.

### Target end state

`solar-logger.ino` holds `setup()`, `loop()`, boot ordering, and the wiring
that lets `autonomous` pump commands without depending on `command`. Nothing
else.

## Automated tests — FOUNDATION BUILT 2026-09-17, CHARACTERIZATION GATE BUILT 2026-09-18

**The pre-modularization characterization gate exists.** As of 2026-09-18,
`uv run pytest` reported 78 passed and 1 xfailed when the gate was built. After
the RELEASE/HOLD correctness fixes the same day it reported 101 passed and 0
xfailed: the xfail became a passing test (D-044). After the scheduler fix, also
the same day, it reported 153 passed, 1 xfailed. The new xfail records the
overrun defect found in that stint, below. The connection stage, still the same
day, added the connection and IntelliSense tests: 183 passed, 1 xfailed, the
same xfail. The NVS stage on 2026-09-23 added 43 tests of what is stored:
226 passed, 1 xfailed, still the same xfail. The telemetry stage and the
DIAG_ALRT correctness work brought it to 333 passed, 1 xfailed. The record
format stage on 2026-09-24 added 54 tests of the module boundary and of the
module's own source compiled on the host: **387 passed, 1 xfailed**, still the
same xfail. The full local gate, including
the warning-free ESP32 compile and the editor database, is:

```text
tools/check.sh
```

`pytest` is a dev dependency in `pyproject.toml`. The plan below is preserved
with its status marked, because the parts that are still unbuilt are still the
plan.

| Module | Covers | Kind |
| --- | --- | --- |
| `tests/test_command_protocol.py` | `CMD_ACK` / `CMD_RESULT` semantics, RELEASE's expected disconnect, missing ACK, missing RESULT, mismatched framing, D-040 waiting rules | real host code |
| `tests/test_session_semantics.py` | `SessionClient` lease state, refused HOLD, failed KEEPALIVE, lease expiry, dropped-ACK recovery, `resolve_wait()` | real host code |
| `tests/test_autonomous_accounting.py` | the running-total rule, and firmware source shape for defects 1, 2 and 5; since 2026-09-18 also "advances exactly once" and the complete writer set | specification + source assertions |
| `tests/test_autonomous_schedule.py` | added 2026-09-18 (D-046): the three pure timing functions compiled from the sketch and run for every sleep path, handoffs, sessions past one cadence and past `micros()`'s wrap, overruns and rollover; the scheduler's and handoff's composition; the timing-state writer sets; one strict `xfail` | firmware code run on the host + source |
| `tests/test_characterization_protocol.py` | `tools/send.sh` exit status and report for 17 wire transcripts (11 at first; 6 added 2026-09-18 for D-044 and D-045); ACK-once and RESULT-once in the dispatcher; RELEASE's completion rule; every handler's result reaching `CMD_RESULT`; the frozen command set and status vocabulary; firmware identity | real host code + source |
| `tests/test_characterization_policy.py` | the defect 4 and defect 6 refusals, and HOLD's refusal while a sleep is pending, all happening before any state change; RELEASE and lease expiry resuming autonomous mode without disarming; the deferred-sleep lifecycle (one arm, one cancel, stop cancels first) | source |
| `tests/test_characterization_record.py` | `AutoRecord` field order, width, signedness and packing; CRC coverage; CRC computed last; validation rule; record constants | source |
| `tests/test_characterization_connection.py` | added 2026-09-18, before the connection move, and unchanged by it: claim, release and the transport bits run on the host, with the exact `[CONNECTION]` wording; HOLD as the only claim; every way out of a session releasing USB; sleep policy asking `anyHostConnected()` and never `isPlugged()` / `isConnected()` | firmware code run on the host + source |
| `tests/test_characterization_nvs.py` | added 2026-09-23 with the NVS move: the namespace, every key name and the 15-character limit, the five checkpoint keys with writer, reader and checked width, `LoggerCheckpoint` field types, failure propagation on both checkpoint paths, power-test defaults, the sequence key and the RTC floor's ordering, and the one-owner boundary | source |
| `tests/test_intellisense.py` | added 2026-09-18 (D-048): a module added to, broken in and deleted from a scratch copy of the sketch, through the real generator and Arduino CLI; every refusal of the database validator, one defect at a time | real tooling + real host code |
| `tests/firmware_source.py` | reads every sketch file as C++ tokens, so source tests survive file moves and reformatting | helper |
| `tests/fake_serial.py` | a scripted stand-in for a pyserial port | helper |

**The source tests were mutation-checked on 2026-09-18.** Applied one at a time
to a scratch copy, 34 regressions each failed the intended test: all six
original defects reintroduced, two same-width record fields swapped, a signed
field made unsigned, packing removed, the CRC range widened, a field written
after the CRC, a disarm on lease expiry, and the rest. Three behavior-preserving
edits left the suite green: every brace reformatted K&R with tabs expanded,
five functions moved into a `.cpp`, and braces placed inside comments and
messages. Details in [LAB_NOTES.md](LAB_NOTES.md).

**What the accounting module does and does not prove** is stated in its own
docstring and is worth repeating here: the wake-cycle math lives in the `.ino`,
which cannot be compiled on the host until modularization, so that module pairs
an executable specification of the rule with assertions against the real
firmware source. It is not a test of compiled firmware behavior. Each of its
regression assertions was verified to FAIL against the pre-fix source before the
fix was applied.

Sleep-deadline arithmetic was built on 2026-09-18, ahead of modularization,
because the scheduler double-count needed it (D-046). Three pure functions stay
inside `solar-logger.ino`, and the test compiles their source on the host
straight out of the sketch. The same technique would reach any other pure
function. Still unbuilt from the original plan: a record/CRC round-trip that
executes the code, and sequence allocation over the D-023 cases. Both touch
functions that are not pure today. Byte offsets and CRC coverage are pinned at
source level, which guards the move but does not execute anything.

### The original plan, for the parts not yet built

Every claim of correctness in this project before 2026-09-17 rested on reading
the source, a warning-enabled compile, `pyright`, and hardware runs written up
in [LAB_NOTES.md](LAB_NOTES.md).

That is a reasonable place to have got to, and it is the wrong place to start a
modularization from. Eleven extraction stages each ending in a hardware check is
a lot of hardware checks, and the things most likely to break during the move —
CRC, record layout, sequence arithmetic, deadline arithmetic — are exactly the
things a laptop can check in milliseconds.

### What is testable where

**A. Pure logic, testable on the host with no hardware and no mocks.** This is
where the value is, and most of it is already written as free functions taking
values and returning values:

- record encode/decode, `autoRecordCrc()`, `autoRecordValid()`, and the
  `static_assert(sizeof(AutoRecord) == 72)` layout contract
- `signExtend20()` / `signExtend40()` and the register-assembly helpers
- the sequence decision in `autoStorageRecover()` — log tail versus NVS floor,
  and `ensureSequenceReservation()`'s "already covered" case
- the sleep-deadline arithmetic in `autonomousDeepSleepAgain()`: remaining time,
  the overrun branch, and rollover behavior
- `AUTO_FLAG_INTERVAL_ODD`'s ±20% window
- `inaModeFromAdcConfig()` / `inaModeIsShutdown()`
- host side: `parse_sample()`, `parse_interval()`, `parse_event()`,
  `requires_live_session()`, `ack_line_for()`, `result_prefix_for()`,
  `capture_seconds_for()`, and `resolve_wait()`'s three-valued rule

**B. Firmware logic needing a host harness.** The register layer is already
behind `readRegisterBytes()` / `writeRegister16()`, so a fake INA228 that serves
register values is enough to drive `runAutonomousWakeCycle()`'s decision tree —
read-before-reset ordering, the append-failure branch, the DIAG_ALRT flags —
without a board. This needs the modularization to have happened first, which is
an argument for doing group A now and group B as the modules land.

**Updated 2026-09-18:** the seam is now `ina228.h`. Both register functions
named above are `static` inside `ina228.cpp`, so a host fake replaces
`ina228.cpp` and implements the header's twelve functions instead. The wake
cycle itself is still in `solar-logger.ino` and cannot be host-compiled yet.

**C. Hardware acceptance.** Irreducible, and already written down: the pending
procedures in [LAB_NOTES.md](LAB_NOTES.md) and the per-stage checks in the
extraction table above. Automation does not replace these; it shrinks how much
they have to cover.

### The smallest foundation worth adding

`pytest` as a dev dependency, a `tests/` directory, and — for the firmware half
— compiling the pure logic as ordinary C++ on the host rather than trying to run
Arduino code. Three of the five tests below need nothing but a C++ file and a
main; the record struct and CRC are already free of Arduino types apart from
`esp_rom_crc32_le`, which a test can supply.

Five first tests, chosen because each one would have caught a defect this review
found by reading, or protects a rule the modularization is most likely to break:

1. **Record round-trip and layout.** Encode a known record, assert the 72-byte
   size, assert every field's byte offset against Section 5 of
   [STORAGE_SYNC_DESIGN.md](STORAGE_SYNC_DESIGN.md), and assert that flipping
   any single byte makes `autoRecordValid()` false. Guards the on-disk format
   through eleven file moves.
2. **Sequence allocation.** Table-driven over the cases D-023 names: intact log,
   empty log, partial tail, corrupt tail, out-of-order sequences, and a
   reservation that already covers the next sequence. Assert the invariant
   directly — a gap is acceptable, a duplicate never is.
3. **Running-total accumulation across a failed append.** Feed two intervals
   where the first append fails, and assert the running total equals the sum of
   the two intervals rather than the first counted twice. This is the
   double-count defect above; it is the test that should be written *with* the
   fix.
4. **Sleep-deadline arithmetic. BUILT 2026-09-18** as
   `tests/test_autonomous_schedule.py` (D-046). Given interval start, cadence, and awake time,
   assert the commanded sleep. Cover the overrun branch and the `uint32_t`
   rollover that the `(int32_t)(target - now) > 0` idiom exists to handle.
   Encodes D-032 and D-034, which are pure arithmetic and currently verifiable
   only on hardware.
5. **Host command classification and protocol strings.** `requires_live_session()`
   over the full command list, `resolve_wait()` over all three flag states
   including the refusal, and `ack_line_for()` / `result_prefix_for()` against
   literal expected text. Cheap, and it pins D-040 and the exact wire strings
   that D-029 says must exist in one place.

Not proposed: a firmware emulator, a serial-protocol integration harness, or CI.
Those are worth revisiting once the modules exist and the fake-INA228 harness in
group B has somewhere to plug in.

---

Recently completed and moved out of this file:

- **INA228 shutdown mode across deep sleep.** Implemented as `POWER TEST SLEEP INA OFF` and measured on 2026-09-10: 0.33 mA against the 1.07 mA INA-continuous baseline, a 0.74 mA saving. See [LAB_NOTES.md](LAB_NOTES.md) and [PROJECT.md](PROJECT.md).

---

# Known bugs and issues

Everything in this section was **confirmed by reading the current source** during
the 2026-09-17 architecture review, except the groups dated 2026-09-18, which
that day's stints found the same way. The scheduler stint also ran the
firmware's timing functions on the host. No hardware was touched, so no entry
here claims a runtime observation. Line references are to the source as of the
stint that recorded them.

The groups below are ordered by when the fix should happen, not by a
severity score. The first group exists because moving this code into modules
would freeze the bad ownership boundary in place rather than expose it.

---

## MUST FIX BEFORE MODULARIZATION - ALL SIX RESOLVED 2026-09-17

**All six were fixed in the correctness stint of 2026-09-17.** The findings are
preserved below exactly as written, each with the resolution appended, because
the reasoning that identified a defect is worth as much later as the fix is.

Compiled clean with `--warnings all`. **Not uploaded, and not verified on
hardware** - the acceptance sequence for that is in [LAB_NOTES.md](LAB_NOTES.md).

| Defect | Resolution |
| --- | --- |
| Running totals double-count on a failed append | committed only inside `if (appended)` |
| Uninitialized `SensorReading` stored as measurements | zero-init plus impossible sentinels (D-042) |
| `CMD_RESULT,...,OK` for failed commands | handlers return `bool`; result means completion (D-041) |
| `POWER TEST STOP` resets autonomous accumulators | refuses while `autonomousOwnsBoard()`; resumes only its own suspension |
| `loadExperimentIdOnly()` maps NVS failure to `exp=0` | returns false; record marked `EXPERIMENT_UNKNOWN` |
| `LOGGER AUTONOMOUS ON` sleeps with an open tethered interval | refused while a host session is held |


### Autonomous running totals double-count when a durable append fails

`runAutonomousWakeCycle()`,
[solar-logger.ino:5767](../Arduino/solar-logger/solar-logger.ino#L5767).

`rtcAutoRunChargeUAh` and `rtcAutoRunEnergyUWh` are advanced by this interval's
charge **before** the record is appended. When the append then fails, the
accumulators are deliberately left unreset so the next interval covers both
periods — which is right for `interval_charge_uAh`, and wrong for the running
totals, which have already absorbed the first period. The next wake reads a
CHARGE register holding both intervals and adds the whole thing again, so the
first interval is counted twice in every subsequent record's `Qsum_uAh`.

Nothing errors. The record that eventually stores is CRC-correct and carries a
running total that is silently too high, forever, for the rest of the session
and for anything the log is later rebuilt from — `autoStorageRecover()` seeds
`rtcAutoRunChargeUAh` from the last valid record's total.

**Direction:** advance the running totals only inside the `if (appended)`
branch, next to `rtcAutoNextSeq++`. The record being built needs the
provisional sum, so compute it into a local, write that into the record, and
commit it to RTC only on a successful append.

**RESOLVED 2026-09-17**, as directed. `provisionalRunChargeUAh` and
`provisionalRunEnergyUWh` are locals; the record carries them; the RTC values
are assigned only inside `if (appended)`. The not-appended branch now also
states that the totals were not advanced, so the two halves of the decision are
visible together in the log. Regression cover in
`tests/test_autonomous_accounting.py`, which asserts both the rule and that the
source contains no compound assignment to either retained total.

### A failed snapshot read stores uninitialized stack values as measurements

`runAutonomousWakeCycle()`,
[solar-logger.ino:5727](../Arduino/solar-logger/solar-logger.ino#L5727).

`SensorReading reading;` has no initializer. `readSensor()` returns false
without assigning any field if an I2C read fails. The failure sets a local
`intervalValid = false` — which is printed and then discarded — and the record
is built from `reading` regardless, so `bus_uV`, `avg_current_uA`,
`avg_power_uW` and `temp_mC` carry indeterminate stack contents.

**No flag bit covers this.** `ACCUM_SUSPECT` is set for accumulator and
DIAG_ALRT failures but not for a snapshot failure, and `intervalValid` never
reaches the record at all. The result is a CRC-valid, correctly-sequenced
record whose instantaneous fields are garbage and whose flags say nothing is
wrong — the exact failure mode D-024 was written about, in a different field.

**Direction:** zero-initialize (`SensorReading reading = {};`) so the fallback
is an explicit zero rather than stack residue, and give the snapshot failure a
flag bit of its own so a host can tell an unread snapshot from a real one. The
interval charge and energy stay valid and the record should still be stored;
only the snapshot is unknown.

**RESOLVED 2026-09-17**, with one correction to the direction above: **there is
no flag bit to give it.** All eight `flags` bits are assigned and the 72-byte
layout is frozen, so a ninth condition cannot be marked without a record schema
change. The struct is zero-initialized and the four snapshot fields carry
physically impossible sentinels instead, which `LOGGER STORAGE DUMP` decodes as
`SNAPSHOT_UNREAD`. The record is still stored, because the interval integral is
the valuable half. Recorded as [DECISIONS.md](DECISIONS.md) D-042, which also
records that the flag byte is now full - the next condition that needs marking
forces a deliberate schema decision rather than another sentinel.

### `CMD_RESULT,<command>,OK` is emitted for commands that failed

`processCommand()`,
[solar-logger.ino:4244](../Arduino/solar-logger/solar-logger.ino#L4244).

Only three commands map their machine result to a real outcome: `SESSION HOLD`,
`SESSION KEEPALIVE` and `SESSION RELEASE`. Every other command falls through to
`emitCommandResult(command, recognized ? "OK" : "ERROR")`, where `recognized`
means only that the dispatcher matched a branch. So the firmware answers `OK`
to, among others:

- `LOGGER AUTONOMOUS OFF` whose NVS write failed and which therefore left the
  board still armed
- `LOGGER AUTONOMOUS ON` that refused because a power test is armed
- `LOGGER INTERVAL 5` that was rejected as out of range
- `LOGGER INTERVAL 60` whose NVS write failed
- `RESET YES` that was refused because accounting is suspended
- `POWER TEST SLEEP` that refused to swap variants
- `POWER TEST STOP` that could not clear a persisted flag
- `LOGGER STORAGE CLEAR YES` that could not format

The host believes it. `SerialDevice.send_command()` sets
`completed = (line == "<prefix>OK")` and `device_tool.py` exits 0 on that, so
`tools/send.sh LOGGER AUTONOMOUS OFF` prints `ALL OK` and exits 0 for a board
that is still armed. This is the 2026-09-11 failure that produced D-022,
reproduced one layer up in the machine protocol that was built to prevent it,
and it contradicts D-036's "bytes-written, parsed, completed ... are never
conflated".

**Direction:** make dispatch return a status instead of `void`. The smallest
version that removes the lie is a file-scope `commandFailed` flag that handlers
set on every refusal and failure path, cleared before dispatch and read at the
single `emitCommandResult` call site; the cleaner version, and the one to take
while extracting `command`, is for each handler to return a `bool` or a small
result enum. Until it is fixed, no host retry loop can trust exit status 0 for
anything but the three session commands.

**RESOLVED 2026-09-17** with the second option: handlers return `bool`. Eleven
functions changed from `void` to `bool` (`resetExperiment()` already returned
one) and the dispatcher collects the result into `commandOk`, which reaches the
single emission site. The contract is recorded as
[DECISIONS.md](DECISIONS.md) D-041 and the build ID moved to
`solar-logger-protocol-ack-v3` so a host can tell the generations apart.

A second, opposite defect on the same contract was found while auditing every
command path: **`LOGGER AUTONOMOUS ON` emitted no result at all when it
succeeded**, because it ended in `esp_deep_sleep_start()`, which does not
return. The host saw `CMD_ACK`, no `CMD_RESULT`, and reported failure for a
board that had armed correctly. It now uses the same bounded deferred-sleep
handshake D-035 built for `RELEASE`. Host-side cover in
`tests/test_command_protocol.py`.

### `POWER TEST STOP` during a USB rendezvous resets the autonomous accumulators

`stopAllPowerTests()`,
[solar-logger.ino:3394](../Arduino/solar-logger/solar-logger.ino#L3394) and
[:3507](../Arduino/solar-logger/solar-logger.ino#L3507).

`accountingWasSuspended` is computed from `intervalAccountingSuspended()`, which
returns true whenever `autonomousOwnsBoard()` does — including
`AUTO_STATE_USB_RENDEZVOUS`. The rendezvous pumps `handleSerialCommands()`, so a
`POWER TEST STOP` typed into a rendezvous reaches `stopAllPowerTests()`, which
runs to the end and calls `resumeIntervalAccounting()` → `resetInaAccumulators()`
even when **no power test was armed at all** (the function says so and then
carries on).

That clears the accumulators the wake cycle reset moments earlier, while
`rtcAutoIntervalStartMs` is left alone. The next autonomous record therefore
reports a full cadence in `interval_ms` against charge accumulated only since
the command. Under-counted, CRC-valid, unflagged.

This is the clearest case of hidden coupling in the file: a power-test stop
path owns measurement state belonging to the autonomous path, and the two only
meet through a shared predicate.

**Direction:** `stopAllPowerTests()` must not touch the accumulators when no
power test was active, and must refuse outright while `autonomousOwnsBoard()`.
Accounting resumption belongs to whoever suspended it. `RESET YES` already
takes the right shape here — it checks `intervalAccountingSuspended()` and
refuses — and `POWER TEST STOP` should too.

**RESOLVED 2026-09-17**, both halves. The function refuses with an explicit
message while `autonomousOwnsBoard()`, and its resume condition changed from
`intervalAccountingSuspended()` - which also answers true for autonomous
ownership it does not own - to the two suspensions it actually created,
`sleepPowerTestRunning` and `intervalAccountingBlocked`. A `POWER TEST STOP`
with nothing armed now says so and leaves the accumulators alone.

The cold-boot maintenance window is deliberately not covered by
`autonomousOwnsBoard()`, so reset-then-stop inside that window remains the
recovery path for a board that somehow holds both flags.

### `loadExperimentIdOnly()` turns an NVS failure into experiment 0

[solar-logger.ino:4728](../Arduino/solar-logger/solar-logger.ino#L4728).

When `preferences.begin(NVS_NAMESPACE, true)` fails, the function sets
`outExperimentId = 0` and returns **true**, with the comment "The namespace has
never been created, so no experiment has ever been started. Zero is the real
answer here, not a fallback."

A read-only `begin()` returning false does mean the namespace is missing — and
it also covers every other reason `nvs_open` can fail. The Arduino `Preferences`
API does not distinguish them, so the comment states as fact something the code
cannot know, and the consequence is that an NVS fault stamps records `exp=0`
with the `EXPERIMENT_UNKNOWN` flag clear.

That is D-024 exactly: "the field was populated, well-formed, CRC-correct, and
wrong." D-024's own rule — "When context cannot be loaded, the record is marked
`EXPERIMENT_UNKNOWN`" — is not honored on this branch.

**Direction:** return false on a failed `begin()` so the record is marked
`EXPERIMENT_UNKNOWN` with `experiment_id = 0xFFFFFFFF`. "No experiment has ever
been started" can be established positively where it matters — a namespace with
no `schema` key already returns 0 through a different branch that really did
open it.

**RESOLVED 2026-09-17**, as directed, with the reasoning written into the source
because it is not obvious: on the path that matters the benign case cannot
occur. Arming is a prerequisite for any autonomous wake, and
`saveAutonomousTestArmed()` opens the namespace read/write, which creates it. By
the time a timer wake calls this function the namespace necessarily exists, so a
failed open is a genuine fault and never a fresh board.

### `LOGGER AUTONOMOUS ON` during a held host session sleeps without closing the interval

`armAutonomousTest()`,
[solar-logger.ino:7053](../Arduino/solar-logger/solar-logger.ino#L7053).

The function checks for an armed power test and for already being armed. It does
not check `hostSessionHeld`. It resets the INA228 accumulators and calls
`autonomousDeepSleepAgain(AUTO_SLEEP_ARM_COMMAND)`, which does not return.

The path is reachable and is one an operator would take: hold a session on an
armed board, send `LOGGER AUTONOMOUS OFF` (D-031 deliberately keeps the session),
then send `LOGGER AUTONOMOUS ON`. The board deep-sleeps immediately. The open
tethered interval is never closed, its charge is discarded by the accumulator
reset, no `CSV_DATA` row is emitted, and the host's lease disappears with no
`RELEASE` and no expiry notice.

Every other route out of a host session goes through
`beginAutonomousSleepFromHostSession()`, which exists precisely to close that
interval (D-026) and to refuse to sleep if it cannot. This one bypasses it.

**Direction:** `armAutonomousTest()` should route through
`beginAutonomousSleepFromHostSession()` when a session is held, or refuse and
tell the operator to `RELEASE` first. Arming is not an accounting-neutral
operation while a session is open.

**RESOLVED 2026-09-17** with the refusal, deliberately rather than the routing.
Adding a second path into autonomous sleep would have meant two functions able
to perform the D-026 handoff, and the value of that handoff is that there is
exactly one of it. The operator is told to `RELEASE` first, and the session is
left untouched.

---

## FOUND 2026-09-18 BY THE CHARACTERIZATION STINT - ALL FOUR RESOLVED 2026-09-18

Confirmed by reading the source while writing the characterization tests. The
characterization stint reported them and fixed nothing. A correctness stint
the same day fixed all four; each finding stays below as written, with its
resolution appended. **Compiled and host-tested only. Not uploaded and not
verified on hardware.** See [DECISIONS.md](DECISIONS.md) D-044 and D-045.

### RELEASE answers `OK` after a failed handoff

`processCommand()`,
[solar-logger.ino:4276](../Arduino/solar-logger/solar-logger.ino#L4276).

The RELEASE branch emits `wasHeld ? "OK" : "NOT_HELD"`, keyed only on whether a
session was held on entry. `hostSessionRelease()` can then fail:
`beginAutonomousSleepFromHostSession()` returns false when
`closeMeasurementInterval()` fails (an I2C read or accumulator reset failure)
or when storage cannot be mounted to rebuild RTC state. On that path the
firmware prints `NOT ALL OK`, refuses to sleep, never prints
`[SESSION] Released: ALL OK`, and stays awake in `IDLE` with autonomous mode
still armed in NVS. The host is still told `CMD_RESULT,LOGGER SESSION RELEASE,OK`.

The consequence is the D-041 failure again, on a failure path: `tools/send.sh`
prints `ALL OK` and exits 0, and `app/solar_logger.py` prints
`[SESSION] Board released: ALL OK`, for a board that is awake at roughly 28 mA
and will stay awake until reset. The 2026-09-17 review listed RELEASE among the
three commands that already reported a real outcome. It reports whether a
session existed, not whether the release completed.

Cover: `test_release_can_report_a_failed_handoff`, a strict `xfail` that
requires the branch to have some status word other than `OK` and `NOT_HELD`,
without choosing which (D-043).

**Recommended deadline: before stage 10 (`host_session`)**, since that stage
moves this exact code, and ideally before modularization starts. The shape that
fits D-041 is for `hostSessionRelease()` to return `bool` like the other
fallible handlers. Whether that changes the build ID is a question for the fix:
the wire vocabulary would gain a use of `ERROR`, not a new word.

**RESOLVED 2026-09-18**, as directed. `hostSessionRelease()` returns `bool`, true
only when the release completed. The handoff returns a `HandoffResult` that
names its failed stage, and the dispatcher answers
`wasHeld ? (released ? "OK" : "ERROR") : "NOT_HELD"`. "Completed" is defined in
D-044. The strict xfail became a passing test. The build ID stays v3 and the
version moves to 0.3.0-dev, for the reasons in D-044.

### HOLD inside the 250 ms deferred-sleep grace is granted, then slept on

`hostSessionHold()`,
[solar-logger.ino:7031](../Arduino/solar-logger/solar-logger.ino#L7031), and
`servicePendingAutonomousSleep()`,
[:6677](../Arduino/solar-logger/solar-logger.ino#L6677).

RELEASE and `LOGGER AUTONOMOUS ON` both set `pendingAutonomousSleep` and return,
and `loop()` sleeps when the grace expires. `servicePendingAutonomousSleep()`
cancels only when autonomous mode is off. `hostSessionHold()` checks only
`autonomousTestArmed`. So a HOLD parsed inside that window is granted: it
answers `OK`, resets the accumulators, opens a tethered interval, and claims
USB. Then the pending sleep fires on top of it. The new interval is discarded
unclosed and the lease vanishes without a RELEASE or an expiry, which is the
defect-6 consequence reached by a different route.

Reachability: the HOLD has to arrive within 250 ms of the RELEASE or arm result.
None of the host tools does that. After a result, `tools/send.sh` keeps
capturing until the port has been quiet for 600 ms or has vanished, which
outlasts the grace, and `app/solar_logger.py` sends HOLD only on connect. A
person typing into a serial terminal, or a future client, could.

**Recommended deadline: stage 10**, with the RELEASE fix above. Either HOLD
refuses while a sleep is pending, or the pending sleep cancels itself when a
session is held. Choosing between them is the fix's decision.

**RESOLVED 2026-09-18 with the refusal.** HOLD answers `ERROR` while any
deferred sleep is pending. Cancel-and-grant was rejected because the RELEASE
handoff has already closed the interval and moved the retained clock (D-044).
The audit of every writer of the pending sleep found two more gaps, both fixed
in the same change. `LOGGER AUTONOMOUS ON` did not enter `DEEP_SLEEP_PENDING`
during its grace, so autonomous did not own the board for those 250 ms. And an
`AUTONOMOUS OFF` whose NVS write failed still let the grace expire into sleep.
Arm and cancel now each have one function.

### Host: the logger reports `Board released: ALL OK` for any RELEASE outcome

`app/solar_logger.py`,
[solar_logger.py:1894](../app/solar_logger.py#L1894), with
`SessionClient.note_line()`,
[device_session.py:648](../app/device_session.py#L648).

`SessionClient` sets `held = False` for every RELEASE result word, and the
logger's shutdown path prints `[SESSION] Board released: ALL OK` whenever
`held` became false, including after a lease-expiry line. A `NOT_HELD` answer
means the firmware had no session left to release, which is not what that line
claims. The preceding `[SESSION] Firmware completed LOGGER SESSION RELEASE:
NOT_HELD` line is accurate; the summary after it is not. Combined with the
firmware defect above, the logger also reports success for a board that stayed
awake.

**Recommended deadline: any time; it is host-only.** The summary line should
come from the result word, not from `held`.

**RESOLVED 2026-09-18.** The logger's release now goes through
`release_session()` in `app/device_session.py`, which reports from the machine
RESULT only (D-045). Fixing it exposed a deeper cause: `SessionClient` never
saw the machine RESULT in the firmware's real line order. The human outcome
line arrives first and cleared the expectation, so the RESULT after it was
ignored for HOLD, KEEPALIVE and RELEASE alike. Human lines no longer consume
the expectation, and a machine `KEEPALIVE ... FAILED` now ends the session by
itself.

### DECISION NEEDED: should SessionClient say when it accepted a RESULT whose ACK was lost?

The 2026-09-18 stint brief stated the invariant "RESULT without a valid ACK:
host must NOT silently treat this as a normal successful exchange". The two
host paths differ:

- `SerialDevice` (the `tools/send.sh` path) refuses: no ACK means exit 1, and
  the `CMD_RESULT` line it saw is printed, not hidden. Pinned by
  `test_send_exit_status_follows_the_command_contract[result-without-ack-is-failure]`.
- `SessionClient` (the logger's session path) accepts it, deliberately. That
  was a fix recorded in [LAB_NOTES.md](LAB_NOTES.md) on 2026-09-17: under
  `setTxTimeoutMs(0)` an ACK can be dropped while the RESULT arrives (D-036),
  and discarding a granted lease was the worse failure. It logs
  `Firmware completed ...: OK`, the same line as a normal exchange, and nothing
  anywhere records that the ACK was missing.

Accepting it is defensible. Accepting it without a word is the silent-deviation
case. Nothing was changed. The cheap option is one warning line when an outcome
arrives for a command still awaiting its ACK, which changes no session state.
Pinned as-is by `test_a_dropped_ack_does_not_discard_the_outcome_that_follows`.

**DECIDED 2026-09-18 as D-045.** One rule for both paths: a RESULT whose ACK was
lost is accepted as the outcome and always flagged. `tools/send.sh` now exits by
the RESULT, so 0 for `OK`, and prints a distinct line and a warning rather
than `ALL OK`. This reverses its previous exit 1, because a retry after a
completed `RESET YES` would start a second experiment. `SessionClient` logs and
counts it. An ACK that follows an accepted result proves the result stale, by
FIFO order, and it is discarded. Session commands get no stricter rule,
because the ACK carries no more identity than the RESULT.

---

## FOUND 2026-09-18 IN THE RELEASE/HOLD CORRECTNESS STINT - ONE RESOLVED, ONE OPEN

Confirmed by reading only. Not fixed in that stint, because each is outside its
three fixes and the first sits in cadence code the stint was told not to
disturb. The original defects were source findings. The scheduler double-count
was fixed later the same day (D-046), and its corrected RELEASE remainder was
subsequently observed on hardware; arming while tethered is still open.

### After a host handoff, the scheduler counts the awake time twice

`beginAutonomousSleepFromHostSession()`,
[solar-logger.ino:6707](../Arduino/solar-logger/solar-logger.ino#L6707), and
`autonomousDeepSleepAgain()`,
[:6364](../Arduino/solar-logger/solar-logger.ino#L6364).

The handoff sets `rtcAutoSessionElapsedMs` to "now" in session time, which
already includes the awake time since `setup()` began. On a timer-wake boot that
is the retained value plus `(micros() - setupEntryMicros) / 1000`; on a cold
boot it is `millis()`. The scheduler then treats `AUTO_SLEEP_HOST_RELEASE` as
continuing the RTC session and computes
`rtcAutoSessionElapsedMs + awakeMs`, where `awakeMs` is again the whole time
since `setup()` began. The awake time `A` is counted twice.

Arithmetic from the code, not a measurement. With `A` below one cadence:

- the commanded sleep is `cadence - A - grace` instead of `cadence - grace`
- the next record's `interval_ms` still reads about one cadence, although only
  about `cadence - A` elapsed. It is CRC-valid, plausible and wrong, and any
  average current derived from it reads low.
- `session_elapsed_ms` stays inflated by `A` for the rest of that boot

Example: a timer wake is claimed during its rendezvous and released 28 s after
`setup()` began. The sleep is commanded for about 31.75 s, and the next record
claims about 60000 ms.

With `A` at or above one cadence, the overrun branch fires instead. It prints a
spurious "overran the interval deadline" warning and sleeps a full cadence; the
next `interval_ms` is right, but the session clock is still inflated by `A`.

Lease expiry takes the same path with no grace.

History: the 2026-09-16 selector change made `AUTO_SLEEP_HOST_RELEASE` continue
the RTC session. Before it, the timer-wake case was wrong and the cold-boot case
was right; now both double-count. Per [LAB_NOTES.md](LAB_NOTES.md), no image
containing that change has been uploaded, so the data recorded so far should
not show this. That is inferred from the notes, not verified on the board.

Consequence for the pending hardware acceptance: the 2026-09-16 check that
RELEASE's sleep diagnostic "reports a remainder near 60000 ms" is predicted to
fail, showing roughly 60000 minus the session length.

**Recommended deadline: before the next upload.** Every RELEASE or lease expiry
on hardware would otherwise write one wrong record into Experiment 3's log.
Direction, for its own stint: measure the handoff's elapsed time from the
handoff itself, so the scheduler adds only the time since then.

**RESOLVED 2026-09-18**, as directed (D-046). The handoff records the `millis()`
instant at which it set the retained clock, in `hostHandoffAtMs`, and the
scheduler's `AUTO_SLEEP_HOST_RELEASE` path adds only the time since then. The
arithmetic above was confirmed by running the firmware's own timing functions
on the host with the defect restored: the 28 s RELEASE commanded 31,750 ms, and
the fix commands 59,750 ms. The cold-boot case double-counted as the entry
says: a release 45 s after `setup()` entry slept 14,750 ms, because `millis()`
is itself the time since boot and the scheduler added the awake time to it.
Two more defects in the same expression were fixed with
it: the handoff's `micros()` wrap after 71.6 minutes, and the rebuild-ordering
item under "SHOULD FIX DURING MODULARIZATION". Cover:
`tests/test_autonomous_schedule.py`. RELEASE subsequently reported 59,750 ms
remaining on hardware, including clean `d017f7d`; explicit lease-expiry timing
remains unverified by the supplied transcripts.

### Arming while tethered discards the open tethered interval without closing it

`armAutonomousTest()`,
[solar-logger.ino:7577](../Arduino/solar-logger/solar-logger.ino#L7577).

On a tethered board with no session held, `LOGGER AUTONOMOUS ON` resets the
accumulators to start the first autonomous interval. It does not close the
tethered interval that was open. The charge since the last `CSV_DATA` row is
discarded, with no row and no statement that it was. The session case was
defect 6 and is refused; this is the no-session case.

**Recommended deadline: any time, low impact.** It loses at most one interval,
once, at arming. Closing the interval first, as the D-026 handoff does, or at
least stating the gap, would fit the existing design.

**Still open after the 2026-09-18 scheduler stint.** That fix does not touch
`armAutonomousTest()`. Arming sets its clock on `millis()` and sleeps through
`AUTO_SLEEP_ARM_COMMAND`, which never had the double count. The two defects
share no code, so this one was left for its own stint rather than folded in.

---

## FOUND 2026-09-18 IN THE SCHEDULER STINT - NOT FIXED

Found while fixing the scheduler double-count (D-046). Confirmed by reading; the
first was also run on the host through the firmware's own timing functions. None
has been observed on hardware.

### An overrun moves the interval boundary without resetting the accumulators

`planAutonomousSleep()` and its caller `autonomousDeepSleepAgain()`.

When awake work reaches or passes the deadline, D-032 says to warn, start the
next interval now, and sleep one full cadence. The code moves
`rtcAutoIntervalStartMs` to now, but nothing resets the INA228 accumulators at
that moment. The wake cycle last reset them, before the rendezvous. So the next
record's charge runs from that earlier reset, while its `interval_ms` runs only
from the moved boundary. An average current or power derived from the record
reads high. The record is CRC-valid, and its `interval_ms` is within 20% of the
cadence, so `INTERVAL_ODD` stays clear. D-018 says a record stores its actual
measured interval; this one does not.

Reachability: every unclaimed wake at `LOGGER INTERVAL 10`, the minimum. The
rendezvous alone lasts 10 s (`AUTONOMOUS_USB_RENDEZVOUS_MS`) and starts after
the reset, so the deadline has always passed by the time the board sleeps. With
the host tests' numbers, the record claims 10,150 ms for about 20,370 ms of
charge. At the 60-second default, an unclaimed wake would have to stay awake
more than 60 s after its reset, and its rendezvous ends at 10 s. A handoff
cannot reach it after D-046, because the handoff itself is the boundary.

Cover: `test_after_an_overrun_the_next_record_covers_what_its_charge_covers`, a
strict `xfail` that states the property without choosing the fix (D-043).
`test_an_overdue_deadline_sleeps_one_full_cadence` deliberately does not pin
where the boundary goes.

**Decision needed.** Two directions:

- Keep the boundary where the accumulators were reset, so `interval_ms` reports
  the long interval truthfully and `INTERVAL_ODD` flags it. Every record stays
  honest at any cadence.
- Refuse cadences shorter than one unclaimed wake, which today means raising
  the minimum above the rendezvous. The overrun becomes unreachable in normal
  operation, but the branch stays wrong.

**Recommended deadline: before `LOGGER INTERVAL 10` is used for data anyone
keeps.** Not blocking at 60 s.

### The host-session timing line still uses `micros()`

`autonomousDeepSleepAgain()`, the `AUTO_SLEEP_HOST_RELEASE` case of the timing
printout. `[TIMING] Host-session duration (setup entry -> sleep)` prints
`awakeUs`, which is `micros() - setupEntryMicros`. After 71.6 minutes it
prints the true duration minus 4,294,967 ms per wrap. It is diagnostic text
only: since D-046 nothing on that path computes with it. Direction: print
`millis() - setupEntryMicros / 1000UL` on that path.

**Recommended deadline: any time, low impact.**

### After a handoff, the new boundary is taken after the interval close, not at its reset

`beginAutonomousSleepFromHostSession()`. `closeMeasurementInterval()` resets
the accumulators, then does a diagnostic I2C read, prints the interval, and
writes the NVS checkpoint. A handoff that has to rebuild RTC state then mounts
storage and scans the log. Only after all of that does the handoff take the new
boundary. The charge accumulated in between goes into the first autonomous
record, and that record's `interval_ms` does not count the time. None of it has
been measured, and the scan is the only part that grows with the log. It
predates D-046, which did not change where the boundary is taken.

Direction: take the boundary at the reset itself. `closeMeasurementInterval()`
already records that instant as `intervalStartMs`.

**Recommended deadline: any time, low impact.**

---

## SHOULD FIX DURING MODULARIZATION

### DIAG_ALRT was read after the accumulators, destroying overflow evidence — FIXED IN SOURCE 2026-09-23, NOT YET HARDWARE VALIDATED

Raised 2026-09-23 as an open ordering question, confirmed the same day against the
datasheet, and fixed the same day. **The source fix is in; the flag has still
never been seen to fire on hardware.**

**What was wrong.** `runAutonomousWakeCycle()` read CHARGE and ENERGY and only
then read `DIAG_ALRT`. SLYS021A Table 7-16 states verbatim that ENERGYOF "Clears
when the ENERGY register is read" and CHARGEOF "Clears when the CHARGE register is
read", so the two accumulator reads cleared both bits before they were sampled.
`AUTO_FLAG_INA_ACCUM_OF` (0x40) could not be set by a genuine accumulator overflow
on the autonomous wake path, and every record asserted "no accumulator overflow"
with a correct CRC over the claim. A wrong belief rather than an error: nothing
failed and nothing was logged.

**What changed.** The single `readRegister16(REG_DIAG_ALRT, diag)` call moved above
`readAccumulatedCharge_mAh()` / `readAccumulatedEnergy_mWh()` in
[runAutonomousWakeCycle()](../Arduino/solar-logger/solar-logger.ino#L3901). The
flag-setting logic is unchanged. Nothing is lost by reading earlier: reading
`DIAG_ALRT` clears none of the three bits, the firmware never writes `DIAG_ALRT`
so ALATCH stays at its reset value 0 (Transparent), and nothing reads CNVRF or the
threshold bits.

`test_characterization_record.py` now pins `diag < charge` and `diag < energy`
inside that function, plus that the flag bodies are unchanged and that D-020's
read-before-reset still holds. The order is the correctness, so it is asserted
rather than commented — and it was verified to fail against the pre-fix source, so
it is a real guard and not a tautology.

**Two side effects, both stated rather than absorbed.** In the case where both the
DIAG_ALRT read and the accumulator reads fail, the two error lines now print in the
opposite order. And the `[TIMING] INA validate+accum read` bucket became
`INA validate+diag+accum`: the DIAG_ALRT read used to be counted in the snapshot
bucket, so those two timing numbers are not comparable across this change. The
label was updated rather than left to mean something different than it says.

**Records already written are not retroactively qualified.** Experiment 3's stored
records still carry `INA_ACCUM_OF = 0` from the broken path, and that zero means
"not measured", not "no overflow". Their charge and energy values are unaffected —
accumulators were always read correctly and before any reset (D-020), and the
40-bit registers are nowhere near overflow at roughly 1.5 mAh per minute — but the
flag cannot be used as evidence for any record below the sequence at which the
fixed image starts running.

**Outstanding, and the datasheet does not substitute for it:** an overflow has
never been induced on hardware, so the corrected path has never been observed to
set the flag. The reorder makes it reachable in principle; only an induced overflow
proves it fires. Needs a hardware check before `INA_ACCUM_OF` is trusted either way.

**Not addressed by this fix.** `MATHOF` (0x20) was never affected by the ordering,
because a read does not clear it. But the datasheet clears MATHOF on "triggering
another conversion" and this firmware converts continuously, so
`AUTO_FLAG_INA_MATHOF` qualifies approximately the most recent conversion rather
than the whole interval. That is a measurement-semantics question and remains open.

Separate from the scheduler-overrun `xfail`.

### Charge is a signed net integral but energy is an unsigned magnitude integral — semantic asymmetry, 2026-09-23

Observed in real Experiment 3 data, not inferred. From the golden record now
fixtured in `tests/test_characterization_record.py`:

```text
I_mA=-0.616   dQ_uAh=-10     Qsum_uAh=380771
P_mW=8.025    dE_uWh=129     Esum_uWh=5602351
```

Current and charge go negative during discharge. Power and energy stay positive
for the same period. So **`running_charge_uAh` can decrease while
`running_energy_uWh` only ever increases**, and the two running totals in one
record are not the same kind of quantity.

**This is the INA228's own behavior, not a firmware defect.** Verified against
SLYS021A:

| Register | Table | Datasheet wording |
| --- | --- | --- |
| CURRENT (7h) | 7-12 | "Two's complement value." |
| POWER (8h) | 7-13 | "Unsigned representation. Positive value." |
| ENERGY (9h) | 7-14 | "Unsigned representation. Positive value." |
| CHARGE (Ah) | 7-15 | "Two's complement value." |

The firmware reads each with matching signedness — `readRegister24Unsigned(REG_POWER, ...)`
and `readRegister40Unsigned(REG_ENERGY, ...)` against `readRegister40Signed(REG_CHARGE, ...)`
— so nothing is being misread. The hardware integrates |P| dt and cannot report
negative power.

**The consequence worth stating:** `running_energy_uWh` is a magnitude integral,
so **net energy balance cannot be computed from these records at all**, and
comparing `Qsum` against `Esum` as though both were net is wrong. A host that
assumes energy is signed will silently conclude the battery gained energy during
a discharge. The record fields are declared `int64_t` / `int32_t`, which suggests
a signed quantity the hardware never produces, and that is the part most likely
to mislead a reader.

**Not changed, and deliberately so.** Fixing this is not a bug fix — it is a
choice among: leave it and document the asymmetry; derive a signed energy on the
host from `sign(dQ) * dE`, which is an approximation whenever current reverses
inside one interval; or reconstruct signed energy on the device, which the INA228
cannot do without integrating V·I in firmware and giving up the hardware
accumulator. The third changes what the instrument measures. **None of these may
be chosen without a decision, and the first is what is in force today.**

Any change to the stored representation is also a 72-byte format change and a
record version decision. See STORAGE_SYNC_DESIGN Section 5.

### The tethered interval close performs no overflow check at all — 2026-09-23

Found while confirming the defect above. `readRegister16(REG_DIAG_ALRT, ...)` at
[solar-logger.ino:3990](../Arduino/solar-logger/solar-logger.ino#L3990) is the
**only** `DIAG_ALRT` read in the entire firmware.

`closeMeasurementInterval()`
[:2786](../Arduino/solar-logger/solar-logger.ino#L2786), which closes every
host-session/tethered interval, reads the snapshot, reads CHARGE, reads ENERGY,
and resets the accumulators without ever consulting `DIAG_ALRT`. So the
`CSV_DATA` interval rows in `data/intervals.csv` carry no overflow qualification
of any kind — not a cleared one, none.

This is a coverage gap rather than a wrong claim: the CSV schema never had an
overflow column, so nothing asserts the interval was clean. Worth resolving in
the same change as the ordering fix, since both want the flag read before the
accumulators. Whether tethered rows should carry the qualification is a schema
decision and is not assumed here.

### One corrupt record discards every valid record after it

`autoStorageScan()`
[solar-logger.ino:5208](../Arduino/solar-logger/solar-logger.ino#L5208) and
`autoStorageTruncateToValid()`
[:5240](../Arduino/solar-logger/solar-logger.ino#L5240).

The scan walks **forward** and breaks at the first record that fails validation.
`trailingBytes` is then everything from that point to end of file, and
`autoStorageRecover()` rewrites the log to the valid prefix automatically at
boot. A single corrupted record in the middle therefore destroys every good
record after it.

STORAGE_SYNC_DESIGN.md Section 11 specifies the opposite: read the final record,
and "walk backwards one record at a time until a valid record is found". For the
power-loss partial tail the two are identical, which is why this has never
shown. For a flash bit-flip they are not, and the implementation is the
destructive one.

The loss is counted and printed, so it is not silent — but it is automatic,
irreversible, and happens before an operator sees the message.

**Direction:** keep scanning past an invalid record to establish how much good
data lies beyond it, and truncate only a genuinely trailing run of bad records.
If valid records exist after an invalid one, report it and refuse to rewrite
without an explicit operator command. Reconcile the code and Section 11 so one
of them stops being wrong.

### The claimed-rendezvous path reconfigures the INA228 and rebases the interval clock

`setup()`, [solar-logger.ino:7685](../Arduino/solar-logger/solar-logger.ino#L7685)
and [:7733](../Arduino/solar-logger/solar-logger.ino#L7733).

When a host claims the rendezvous, `hostSessionHold()` establishes the tethered
baseline — accumulator reset, `intervalStartMs = millis()` — and `setup()` then
falls through into ordinary initialization. Two things there undo part of it:

- `intervalStartMs = millis()` runs again unconditionally at :7685, so the
  interval clock restarts some tens of milliseconds after the accumulator
  baseline it is supposed to match. The `hostSessionBaselineEstablished` guard
  further down correctly skips the second accumulator reset, but by then the
  timestamp has already moved. `[SESSION] Tethered interval open for` is
  measured from the wrong origin, and the eventual close divides real charge by
  a slightly short elapsed time.
- `configureIna228()` at :7733 writes `ADC_CONFIG`. D-020: "A wake must not
  reconfigure an INA228 that is already configured and already converting.
  Writing the `ADC_CONFIG` MODE bits interrupts and restarts a conversion in
  progress." Up to one conversion period (~202 ms at the current settings) of
  the freshly-started tethered interval is discarded with no statement.

Both are small. Both are the kind of ordering dependency that becomes invisible
once `setup()` is calling into modules.

**Direction:** make the baseline guard cover the timestamp as well as the
accumulator reset, and gate `configureIna228()` on "the device is not already
correctly configured and converting" — `validateInaForWake()` already answers
that question with reads only.

**Still open after the INA228 extraction (2026-09-18), and unchanged by it.**
`configureIna228()` now lives in `ina228.cpp`, but the decision to call it is
in `setup()`, and so is the fix. The driver knows nothing about a claimed
rendezvous and should not learn it (D-047). The line references above predate
the extraction.

### NVS load failures become plausible defaults with no signal

`loadAutonomousSettings()`
[solar-logger.ino:4791](../Arduino/solar-logger/solar-logger.ino#L4791) and
`loadSequenceHighWater()`
[:4901](../Arduino/solar-logger/solar-logger.ino#L4901).

- `loadAutonomousSettings()` returns true after a failed `begin()`, having set
  `autonomousTestArmed = false` and the default cadence, and prints **nothing**.
  An armed board that cannot read NVS reports itself disarmed and behaves that
  way. Both call sites ignore the return value anyway.
- `loadSequenceHighWater()` returns 0 on a failed `begin()`, which is
  indistinguishable from "no reservation has ever been written". The NVS floor
  that D-017 and D-023 rely on to make duplicates impossible collapses to zero
  on exactly the failure where it is needed.

The power-test loaders in the same file get this right: they print an explicit
"flag unavailable; defaulting to not armed" line. These two do not.

**Direction:** distinguish "read successfully, value absent" from "could not
read". A failed high-water read must not be allowed to lower the floor; keeping
the previous RTC value or refusing to allocate a sequence are both better than
returning 0.

**STILL OPEN after the NVS extraction of 2026-09-23.** Both NVS read operations now live
in `Arduino/solar-logger/nvs_persistence.cpp`; the line references above are to
the pre-extraction sketch. `loadAutonomousSettings()`'s read half is
`loadAutonomousConfig()` there, and `loadSequenceHighWater()` kept its name and
its body. Neither was fixed, deliberately: a behavior change must not ride along
inside a move. Both are now stated at the top of `nvs_persistence.h`, so a
caller reading the header is told rather than having to find this entry. The fix
is cheap now that one file owns the reads — it needs a way to say "could not
read" that the callers then have to handle, which is the behavior change that
needs its own stint.

### `rtcAutoMagic` is set before the expression that tests it

`beginAutonomousSleepFromHostSession()`,
[solar-logger.ino:6297](../Arduino/solar-logger/solar-logger.ino#L6297).

The RTC-rebuild branch assigns `rtcAutoMagic = AUTO_RTC_MAGIC` and then, a few
lines later, computes `sessionElapsedNow` from a ternary whose condition
includes `rtcAutoMagic == AUTO_RTC_MAGIC`. Inside that branch the test can no
longer fail. The rebuild also does not reset `rtcAutoSessionElapsedMs` or
`rtcAutoIntervalStartMs`, so if the condition's other half
(`bootWakeupCause == ESP_SLEEP_WAKEUP_TIMER`) were ever true here, the session
clock would continue from RTC contents that had just been declared invalid.

No reachable path reaches it today: `setup()`'s timer-wake branch handles RTC
loss itself and sleeps. The review could not construct a case that gets here
with a timer wake cause and an invalid magic.

**Direction:** capture the validity into a local before the rebuild writes the
magic, and have the rebuild set the session clock explicitly the way the
`setup()` RTC-loss path does. A latent ordering dependency is the worst kind of
thing to carry across a file split.

**RESOLVED 2026-09-18** with the scheduler fix (D-046), as directed. The fix
rewrote this expression anyway, so the ordering was corrected there rather than
carried forward. `continuesRetainedClock` is captured before the rebuild. A
rebuilt handoff then sets the session clock to this boot's `millis()`, the same
clock the RTC-loss path uses. `test_source_handoff_sets_the_clock_and_the_instant_together`
pins the order, and the rebuilt case runs in
`test_a_session_this_boot_began_runs_on_this_boots_clock`.

### Return values ignored on paths that disarm or persist state

Several places treat a persistence failure as done:

- `autonomousDeepSleepAgain()`
  [:6148](../Arduino/solar-logger/solar-logger.ino#L6148) — on a wake-timer
  failure it calls `saveAutonomousTestArmed(false)` and ignores the result, and
  clears `autonomousTestRunning` while leaving `autonomousTestArmed` true, so
  RAM and NVS disagree in both directions.
- `armAutonomousTest()` [:7128](../Arduino/solar-logger/solar-logger.ino#L7128)
  — same pattern on the clean-first-interval failure path.
- `processCommand()` [:4014](../Arduino/solar-logger/solar-logger.ino#L4014) —
  `emitCommandAck()` returns whether the bounded writer met its 100 ms deadline;
  the result is discarded. `RELEASE` checks the equivalent for its result line
  and prints either outcome, which is the pattern the others should follow.
- `ensureSequenceReservation()` in the wake cycle
  [:5853](../Arduino/solar-logger/solar-logger.ino#L5853) — the underlying
  `reserveSequenceBlock()` prints its own errors, so this one is not silent, but
  the cycle continues as though the reservation had been written.

**Direction:** these are cheap individually and worth doing as each owner moves.
The `armed` flag in particular should have exactly one rule: RAM follows NVS, and
a failed write leaves both alone and says so.

---

## BACKLOG / LATER

### There is no storage-full policy

`autoStorageAppend()`
[solar-logger.ino:5466](../Arduino/solar-logger/solar-logger.ino#L5466) never
consults free space. `LittleFS.totalBytes()` and `usedBytes()` are read only by
`printStorageInfo()`, a human-facing command.

Source-only failure analysis: when an append fails or short-writes, the firmware
prints storage errors, leaves the accumulators unreset, and does not advance
retained totals. There is no persistent storage-full state or automatic
reclamation; an unattended board has no host to retain those diagnostics. A
full-partition hardware test has not been recorded.

**Updated 2026-09-17.** This entry used to end "and the running-total
double-count above then fires on every subsequent wake, permanently". That part
is fixed: the retained totals are now committed only after a successful append,
so a full partition no longer corrupts the running totals as well as stopping
the log. The remaining gap is persistent full-state reporting and recovery,
not the absence of a serial error message.

STORAGE_SYNC_DESIGN.md Section 9 proposes a policy that is not yet accepted: reclaim
acked records first, then overwrite the oldest unacked and count it in a
persistent `dropped_unacked_count`, with the event printed loudly. None of
`INFO`, `SYNC FROM`, `ACK`, `last_acked_seq`, reclamation or
`dropped_unacked_count` exists as a storage-sync API/state in the firmware.
`LOGGER STORAGE INFO` and command-protocol `CMD_ACK` do exist; neither is a
storage-sync acknowledgement.

This has a date attached rather than being hypothetical. Section 6 computes
about 18,000 records on the current partition, which is roughly 12.5 days at the
60-second cadence. The 2026-09-23 hardware snapshot of Experiment 3 was **not
full**: 1,196,032 bytes free, 3,272 valid records, intact tail. The displayed
16,611-record / 11-day remainder is a free-space estimate at that moment, not a
measured exhaustion date or a fresh reading.

**Direction:** this is the store-and-forward milestone, and it is now the
limiting factor on how long the logger can be left alone. Until it is built, a
free-space check that refuses to arm — and that warns during the cold-boot
window when the log is above some fraction of the partition — costs little and
converts a silent stop into a stated one.

### DECISION NEEDED before any of it is implemented

**Nothing was implemented here on 2026-09-17, deliberately.** The correctness
stint was asked whether one of the six MUST-FIX defects covered storage-full. It
does not: this item is in BACKLOG / LATER and always has been.

The policy it would implement is **Section 9 of
[STORAGE_SYNC_DESIGN.md](STORAGE_SYNC_DESIGN.md), which is marked PROPOSED, not
accepted.** Section 9 says so itself: "This is a policy choice that should be
confirmed rather than assumed." Implementing a proposal as though it were a
decision is the deviation failure this project exists to avoid, so the choice is
surfaced here instead.

Three things need deciding together, because the second and third only make
sense once the first is answered:

1. **Which policy.** Section 9 recommends: reclaim acknowledged records first;
   if still full, overwrite the oldest unacknowledged record and count it in a
   persistent `dropped_unacked_count`. The alternative it names is to stop
   logging, which preserves the oldest unsynced data and goes silent exactly
   when nobody is watching. Section 9 recommends the first and does not decide
   it.
2. **Whether reclamation can exist before ACK does.** "Reclaim acknowledged
   records first" presupposes `last_acked_seq`, which presupposes the `ACK`
   half of the sync protocol. None of `INFO`, `SYNC FROM`, `ACK`,
   `last_acked_seq` or `dropped_unacked_count` exists in the firmware today. A
   drop policy without acknowledgement can only ever drop unsynced data.
3. **What pruning does to D-023.** That decision says an intact log tail is the
   sequence authority *only while records are never removed*, and names this
   exact consequence: "Once pruning exists, a pruned log no longer carries the
   highest sequence ever used, and the NVS floor has to apply unconditionally
   again." Any reclamation therefore changes the sequence rules at the same
   time.

A free-space warning is separable from all three and carries no policy, which is
why it is the one part that could be built without this decision.

### The firmware cannot measure its own RTC drift

`autonomousDeepSleepAgain()` pre-advances the session clock by the commanded
sleep before sleeping
([:6078](../Arduino/solar-logger/solar-logger.ino#L6078)), and
`runAutonomousWakeCycle()` derives both `session_elapsed_ms` and `interval_ms`
from that pre-advanced value plus measured awake time. The source says so in a
comment: the interval duration "is not an independent measurement of elapsed
wall time".

So `interval_ms` reporting 60003 ms does not mean the sleep took 60003 ms. It
means the firmware commanded 60000 ms and measured 3 ms of awake work, and every
record will say that regardless of what the RC oscillator actually did.
`rtcAutoCommandedSleepMs` is retained across sleep and never read, so not even
a stored comparison exists.

STORAGE_SYNC_DESIGN.md Section 12 question 3 — RTC drift on this board, the
question that gates the `HOLDOVER` threshold, retrospective anchoring, and
whether an external RTC is ever justified — therefore cannot be answered from
the log alone. It needs a host present across at least two wakes, comparing
`captured_at` deltas against `interval_ms`, or a device-side absolute-time
reference that does not yet exist.

**Direction:** record it as a constraint on the drift experiment rather than a
firmware bug. When `SET_TIME` lands, storing both the commanded sleep and the
wall-clock delta across a sleep makes the measurement fall out of normal
operation.

### The seven FATAL halt loops strand an armed board awake

`setup()` contains seven `while (true) { Serial.println(...); delay(5000); }`
blocks — INA228 identity, INA228 configuration, `loadCheckpoint()`, both
power-test flag loads, the variant load, the initial `readSensor()`, and the
initial accumulator reset.

`loop()` is never reached from any of them, so `handleSerialCommands()` never
runs and the board cannot be talked to at all. An armed autonomous board that
trips one of these sits awake at roughly 28 mA — against a 1.07 mA sleep floor —
until someone physically resets it, and it stops recording without any durable
trace.

Halting is the right answer on a bench with a person watching. It is the wrong
answer for a logger in a car.

**Direction:** part of the deployment-mode work rather than a fix on its own.
The shape that fits the existing design is a bounded diagnostic window that
parses commands, followed by a deep sleep and a retry on the next wake, with the
failure recorded durably so the gap is explainable afterwards.

### Host: a failed HOLD write leaks the serial port

`app/solar_logger.py`, the connect block around
[solar_logger.py:1506](../app/solar_logger.py#L1506).

`session.request_hold()` runs inside the same `try` as `serial.Serial(...)`. If
the HOLD write raises `SerialException` — a board that slept between open and
write, which is exactly the rendezvous case — the handler sets `ser = None`
without calling `close()`. The OS handle is dropped rather than closed, and the
next discovery pass can find the port busy.

**Direction:** close the port on that path, as every other disconnect path in
the loop already does.

### Host: the logger-owned command path exits 0 without an acknowledgement

`app/device_tool.py`, `queue_for_logger()`
[device_tool.py:124](../app/device_tool.py#L124).

The module docstring defines exit 0 as "the firmware acknowledged and completed
the command". When `app/solar_logger.py` owns the port the command is written to
`.serial-command` and the function returns 0, having printed that no
acknowledgement was observed.

The printed text is honest and matches D-022. The exit status is not, and it is
the exit status a retry loop reads.

**Direction:** give the queued path its own status — a distinct non-zero code,
or a documented exception to the table. Whichever, the docstring and the code
have to agree.

### Documentation describes tunables that no longer exist

Three confirmed drifts, all in the direction of promising behavior the code does
not have:

- [PROJECT.md](PROJECT.md) documented `SEND_RESPONSE_SECONDS`,
  `SEND_RESPONSE_SECONDS_DUMP` and `SEND_RESPONSE_IDLE_MS` as environment
  variables that `tools/send.sh` clamps "with a notice rather than silently
  accepted". `grep` finds them in no script and no module. They were lost when
  `send.sh` became a wrapper around `device_tool.py`, which takes
  `--capture-seconds` and `--idle-seconds` instead. **Corrected in PROJECT.md on
  2026-09-17.**
- `tools/upload.sh` read `UPLOAD_PORT_POLL_SECONDS` and
  `UPLOAD_WAIT_HEARTBEAT_SECONDS` into variables that nothing used; waiting had
  moved to `device_tool.py wait-port`, which honored neither. **RESOLVED
  2026-09-17 by wiring them through** rather than deleting them: both were
  documented tunables that had regressed to doing nothing, so restoring the
  behavior was the fix and removing the knobs would have been a second silent
  change. `wait-port` now takes `--poll-seconds` and `--heartbeat-seconds`.
- `tools/upload.sh` defined `find_port()` and never called it. **RESOLVED
  2026-09-17 by deletion** — genuinely dead, with the `find-port` subcommand it
  wrapped still available from `device_tool.py` for anyone who needs it.

### Host watchdog is not aware of power-test telemetry cadence

During `POWER TEST SLEEP` the firmware intentionally suspends `CSV_SAMPLE` and `CSV_DATA` and emits only the 5-second heartbeat. The Python logger's silence watchdog warns after 3 seconds (`SILENT_WARNING_SECONDS` in `app/solar_logger.py`), so every gap between heartbeats trips it.

The observed result during the 2026-09-10 measurement was a repeating pair on every awake phase:

```text
[SERIAL] State: CONNECTED-BUT-SILENT
[SERIAL] Firmware data resumed: ALL OK
```

This is a diagnostic-state bug, not a measurement failure and not a serial fault. Nothing was lost. The 10-second `SILENT_DISCONNECT_SECONDS` threshold is never reached, so the port is never actually dropped over it; the warnings are noise that trains the operator to ignore a watchdog that exists to catch a real ROM-downloader or dead-sketch condition.

Note that the disconnect during the 30-second deep sleep itself is genuine — the USB device really does disappear. Only the in-awake-phase warnings are false.

Fix later by making the watchdog aware of intentional power-test telemetry cadence and state, so its expectations track what the firmware is actually supposed to be sending. Not scheduled.

### Bind SESSION KEEPALIVE and RELEASE to the session they were meant for

`tools/send.sh` refuses to wait for a rendezvous before sending a session-scoped
command, which removes the long window in which a stale RELEASE could land on
somebody else's session (D-040). It does not remove the short one: a RELEASE
typed now, against a board that is present now, is sent even if the session it
was meant for ended and a different one began.

The stronger rule would record the port and the time of each successful HOLD
and KEEPALIVE, and require a KEEPALIVE or RELEASE to match the recorded port
and be inside the firmware's 15-second lease.

Not built, because it introduces host-side session state that can itself go
stale, and because `app/solar_logger.py` holds its sessions through
`SessionClient` rather than through `device_tool.py`, so the record would cover
only some of the sessions that exist. Worth doing if a real collision is ever
observed, and not before.

---

# Experiments to run

## Deep-sleep wake transient

The DMM briefly displayed overload around some wake transitions during both the 2026-09-10 runs, so peak current is uncharacterized. Needs an instrument faster than a handheld meter.

## Wi-Fi transient current

The steady STA-idle delta is measured; burst behavior is not. The DMM was on its 10 mA range, which may average out short ESP32 current bursts.

## Cold-start versus post-sleep awake current

Cold start read approximately 28.7-28.8 mA while post-deep-sleep awake phases read approximately 28.3-28.4 mA, a difference of roughly 0.4 mA. Both boots run the same `setup()` path and no cause has been established. Worth its own test only if it matters to a power budget.

## INA228 consumption in the awake logger

The old `28.4 mA - 18.6 mA = 9.8 mA` subtraction is marked NOT VALID in [LAB_NOTES.md](LAB_NOTES.md), because removing the INA228 also halts the firmware into its FATAL loop.

The 2026-09-10 shutdown measurement gives a much better number, but not quite this one: 0.74 mA is the INA228's continuous-conversion draw **with an idle I2C bus**, since the ESP32 was in deep sleep in both halves of that comparison. The datasheet notes that active clock and data activity increases consumption as a function of bus frequency, so the figure in the awake logger — which polls at 1 Hz — should be somewhat higher. Measuring that needs two awake states differing only in the INA228's mode.

---

# Longer-term ideas

**Nothing in this section is committed, scheduled, or planned.** These are concepts with a rationale, recorded so they are not lost and not mistaken for work.

## Adaptive day/night power mode

Possible future behavior, driven by the measured 0.74 mA cost of keeping the INA228 converting through ESP32 sleep.

**Explicitly out of scope for the first autonomous-storage implementation.** It must not shape that design. The one thing the storage design owes it is that cadence is a setting rather than a constant (D-018), which makes adaptive cadence possible later without a rewrite.

**Day mode:**

- INA228 remains in continuous conversion.
- ESP32 spends most of its time in deep sleep.
- Hardware CHARGE/ENERGY accumulation continues.

**Night detection:**

- After solar current remains near zero or negative for a configurable period, infer that meaningful solar production has ended.

**Night mode:**

- Put the INA228 into shutdown.
- ESP32 deep sleeps.
- Periodically wake at a much lower cadence, for example every 5-15 minutes.
- Restore and configure the INA228.
- Allow measurements to settle.
- Sample solar current and voltage.
- If meaningful charging has returned, transition back to day mode.
- Otherwise shut the INA228 down and sleep again.

**Potential benefit:** measured sleep current could fall from approximately 1.07 mA to 0.33 mA for much of the night.

**Information tradeoffs:**

- Continuous nighttime reverse-current measurement would be lost.
- Hardware CHARGE/ENERGY accumulation would stop during INA shutdown, leaving a hole in the charge record for the whole night rather than a gap of known length.
- Sunrise would only be detected on a polling wake, so the transition back to day mode is late by up to one polling interval.
- Transitions need hysteresis and debounce so passing clouds or brief low-light conditions do not constantly switch modes. A naive threshold would thrash at dawn, dusk, and under broken cloud.

A hardware-based solar-presence or wake signal might allow better detection without periodic polling. That would need additional design and additional hardware, and it is only an idea at this stage — nothing has been specified, costed, or breadboarded.

## External battery-backed RTC — conditional, not planned

**Only worth considering if we later decide accurate absolute time must survive total logger power loss.**

The ESP32-C3 carries wall-clock across deep sleep through its RTC domain, but that domain loses power with the board, so a battery disconnect leaves the logger with no absolute time until the next sync. The design handles that honestly: records written before a sync are marked `UNKNOWN` rather than given a fabricated timestamp, and `seq` keeps everything correctly ordered regardless. See [DECISIONS.md](DECISIONS.md) D-019.

An external battery-backed RTC would remove that gap and would also sidestep the drift of the internal RC oscillator. It costs parts, wiring, and a battery that itself eventually dies.

Two things should be measured before this is even evaluated: actual RC drift on this board, and whether wall-clock genuinely survives deep sleep here as the build configuration suggests. Both are listed in [STORAGE_SYNC_DESIGN.md](STORAGE_SYNC_DESIGN.md) Section 12. Adding hardware to fix an unmeasured problem is the wrong order.

## Optional Wi-Fi/NTP — later, not an installed dependency

Phone/BLE time and sync are now the intended installed path (D-053), tracked in
the installed-system work above. Automatic USB `SET_TIME` is a possible bench
aid rather than the first required installed implementation. Wi-Fi/NTP could
later feed the same transport-independent time contract, but no home Wi-Fi
proximity or ESP-to-server connection is assumed. Neither BLE time-setting nor
Wi-Fi/NTP time-setting is implemented.

## Hardware power-gating of the INA228

Cutting INA228 supply rather than using its shutdown mode. The 2026-09-10 measurement makes this much less attractive than it looked beforehand: shutdown already recovered 0.74 mA of the 1.07 mA, and the INA228's own shutdown draw is specified at 2.8 µA typical, so gating the rail can recover at most a few microamps more.

It also costs a board change, and it loses the property that shutdown is a reversible register write with no external parts.

## Store-and-forward telemetry logger — DESIGN DONE, storage confirmed on hardware

Autonomous sleep/read/store is implemented as `LOGGER AUTONOMOUS ON/OFF/STATUS`; `LOGGER TEST ...` names are deprecated aliases (D-031). Its first hardware run on 2026-09-11 produced eight durable 72-byte records. The latest supplied 2026-09-23 storage snapshot has 3,272 valid records and an intact tail. Sync, ACK, pruning, and the production ring buffer are all still unbuilt; see [STORAGE_SYNC_DESIGN.md](STORAGE_SYNC_DESIGN.md) Section 12a for what exists and Section 13 for what comes next.

The design pass is done. See [STORAGE_SYNC_DESIGN.md](STORAGE_SYNC_DESIGN.md) for the partition audit, record format, capacity and wear arithmetic, wake ordering, timestamp strategy, sync protocol, and recovery design. Accepted decisions from it are D-017 through D-020 in [DECISIONS.md](DECISIONS.md).

The background below is preserved because it is what motivated the design.

### Historical motivation — before autonomous storage

- Detailed sample and interval history exists only when the host Python logger is connected and writing CSV.
- The ESP32 persists experiment and accounting state to NVS, but not full telemetry history.
- Serial output produced while the host is absent is lost.

The remaining serial-history gap is visible in two places already recorded elsewhere. `CSV_EVENT` messages emitted during `setup()` are normally lost, because the host has not finished rediscovering the re-enumerated USB device yet; that affects `EXPERIMENT_RESUME` and, during the deep-sleep power test, `SLEEP_TEST_WAKE`. And the deep-sleep power test itself produces no durable telemetry at all while it runs, which is separate from the autonomous mode that now stores intervals locally.

### Future direction

- The ESP32 should persist timestamp or sequence-based telemetry locally.
- A later host, Bluetooth, or Wi-Fi connection should sync the unsent records.
- The host should acknowledge received records.
- Storage reclamation should be a separate operation from experiment reset. Freeing space must not be reachable only by destroying experiment state.
- NVS is likely appropriate for state and checkpoints, not for a full long-term time series.
- Investigate LittleFS or another flash-backed append-only format.

Status update: LittleFS local records and sequence recovery are implemented.
The sync/ACK/reclamation portions remain unbuilt; the background above is the
original motivation, not a claim that autonomous storage is still absent.

## Installed access versus the current maintenance window

The 15-second cold-boot maintenance window (D-021) is implemented for development
recovery. D-054 now settles the installed direction: a permanent manual physical
SYNC/wake path followed by BLE phone access, with button circuitry and awake
window details still to implement. This is no longer merely a choice between a
button and BLE; the physical wake makes the radio available. No remote wake of
a fully sleeping radio is assumed.

Future vehicle-on wake is separate and does not replace manual SYNC. Its signal
is unchosen and belongs with IBS/F22 wiring research above. USB electrical
presence still does not count as a host claim (D-025), and the current USB
management workflow remains unchanged.

## Battery state of charge

The application does not display or claim to measure battery state of charge, and the cumulative-charge chart is charge through the INA228-measured solar path only. Future SOC work should consider BMW IBS data and/or a rested-voltage model. See [DECISIONS.md](DECISIONS.md) D-009.
