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

**Four modules exist**, each extracted with no intended behavior change:

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
  See the 2026-09-23 LAB_NOTES addendum for limits. Its boundary is D-050. This is the `nvs_rtc` row of the module table below,
  **NVS only** — the RTC-retained state that row also names did not move and
  belongs to `auto_state`.

- **Telemetry**, `telemetry.h` and `telemetry.cpp`, 2026-09-23: how each
  machine-readable CSV line is spelled, and nothing about when one is emitted.
  Compiled and host-tested; it has **not** run on hardware. Its boundary is
  D-051. This is the `telemetry` row of the module table below, built as that
  row describes.

Telemetry was the fourth stage chronologically and is numbered 5 in the
original table below, because that table split the INA228 work into two rows.
Those plan numbers are not the chronological stage numbers.

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
| `rtcSleepTestMagic`, `rtcSleepTestCycle` | R | `armSleepPowerTest()`, `enterSleepPowerTestDeepSleep()`, `stopAllPowerTests()`, cold-boot resume | Belong to `power_test`, not `auto_state`. Guarded by their own magic (D-011). |
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

### Proposed modules

Named after what owns the state rather than after the existing section
comments. The two splits that make the graph acyclic — `auto_state` /
`auto_orchestrator` and `connection` / `host_session` — are why the list is
eleven modules rather than nine; the next section shows the measurement that
forced them.

| Module | Owns | Depends on |
| --- | --- | --- |
| `nvs_rtc` | **NVS HALF BUILT 2026-09-23** as `nvs_persistence.h` / `.cpp`: the single `Preferences` object, the namespace, every key and every typed get/put. The name in this row is misleading and the split was made deliberately (D-050): RTC-retained state is a different store with a different lifetime, and it stays for `auto_state` | Arduino |
| `connection` | **BUILT 2026-09-18** as `connection.h` / `connection.cpp`: `activeTransports`, `anyHostConnected()`, claim/release, `printActiveTransports()`. The USB presence diagnostics stayed in the sketch, because presence is not a claim (D-049) | Arduino |
| `auto_state` | `AutoState`, the ten `rtcAuto*` RTC globals, `setAutoState()`, `autonomousOwnsBoard()`, LittleFS mount, 72-byte record encode/CRC/append, tail scan, sequence reservation | `nvs_rtc` |
| `ina228_regs` | **Merged into `ina228`, built 2026-09-18.** I2C register read/write, sign extension | Arduino, `Wire` |
| `ina228` | **BUILT 2026-09-18** as `ina228.h` / `ina228.cpp`: the register layer, identity, configuration, shutdown, `readSensor`, accumulator read/reset. `inaShutdownActive` stayed in the sketch (D-047) | Arduino, `Wire` |
| `telemetry` | **BUILT 2026-09-23** as `telemetry.h` / `telemetry.cpp`: `CSV_HEADER` / `CSV_SAMPLE` / `CSV_DATA` / `CSV_EVENT` formatting. `CSV_HEADER` was not in this row and moved with `CSV_DATA`, which it describes. No state, and no dependency beyond Arduino, which is why the row structs repeat the measurement fields rather than taking a `SensorReading` (D-051) | Arduino |
| `experiment` | `experimentId`, `completedInterval`, running totals, `intervalStartMs`, checkpoint save/load, interval close, accounting-suspension predicate | `ina228`, `nvs_rtc`, `telemetry`, `auto_state` |
| `power_test` | Wi-Fi test, both deep-sleep variants, arm/stop/status | `ina228`, `nvs_rtc`, `experiment` |
| `auto_orchestrator` | wake cycle, USB rendezvous, deadline scheduler, cold-boot window, host handoff, arm/stop/status, storage commands, `validateInaForWake()` | `auto_state`, `connection`, `experiment`, `ina228`, `nvs_rtc` |
| `host_session` | the lease: `hostSessionHeld`, deadlines, keepalive accounting, HOLD/KEEPALIVE/RELEASE, deferred-release sleep, `serviceHostLease()` | `connection`, `auto_orchestrator`, `experiment` |
| `command` | tokenizing, `CMD_ACK`/`CMD_RESULT`, the bounded protocol writer, dispatch | everything below it |
| `solar-logger.ino` | `setup()`, `loop()`, boot ordering, the `pumpCommands` wiring | everything |

`ina228_regs`, `ina228`, `telemetry` and `experiment` are shown separately
because they are separate files worth having; the measured stack in the next
section treats them as one `measurement` level, which is where they all sit.

The first two were built as one module on 2026-09-18. Outside the driver, the
only register-layer function anything calls is `readRegister16()`, used by
`validateInaForWake()` and the wake cycle's DIAG_ALRT read. A separate
`ina228_regs` would have had to export every register helper to `ina228`. As one
module, nine of the ten are `static` and only `readRegister16()` is public.

### Dependency direction — CORRECTED 2026-09-16

An earlier version of this plan claimed "exactly one hard cycle, `command` ↔
`autonomous`." **That was wrong.** A full call-graph pass over all 119
functions, with comments and string literals stripped, found **three** mutual
pairs, and all three run through autonomous scheduling:

| Boundary | down | up |
| --- | ---: | ---: |
| `commands` ↔ `autonomous scheduling` | 6 | 2 |
| `host sessions` ↔ `autonomous scheduling` | 8 | 8 |
| `measurement/accounting` ↔ `autonomous scheduling` | 3 | 8 |

### Why every arrow through autonomous looked bidirectional

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

### What the splits actually achieve — measured, not asserted

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

### Resulting dependency stack

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

### Upward calls worth fixing while moving, not after — CORRECTED 2026-09-18

This section used to list two, both in the `measurement` → `auto_orchestrator`
direction. **Only one is real:**

- `intervalAccountingSuspended()` and `intervalAccountingSuspendReason()` call
  `autonomousOwnsBoard()`. Harmless once that moves to `auto_state`, which is
  below measurement.

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

### Extraction order, lowest risk first

Bottom of the measured stack first, so nothing ever moves before the things it
calls. Each stage compiles and uploads on its own. **Do not batch them.**

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

The remaining original plan is below; these later stages have not been
executed.

| Stage | Move | Why / boundary | Hardware check afterwards |
| ---: | --- | --- | --- |
| 6 | `auto_state` | **the split. Read-only first, append last.** Carries the ten `rtcAuto*` RTC globals | `LOGGER STORAGE INFO` and `LOGGER STORAGE DUMP` decode the existing log with the same record count and an intact tail |
| 7 | `experiment` | 8 globals; everything it calls has moved | a full 60-second interval closes with the same running totals |
| 8 | `power_test` | self-contained; nothing else calls into it | arm and stop each variant; confirm the refusal to arm two |
| 9 | `auto_orchestrator` | the other split, plus the `pumpCommands` injection | five consecutive unclaimed records at the configured cadence; then `LOGGER AUTONOMOUS OFF` inside the cold-boot window, which is the edge being inverted |
| 10 | `host_session` | the lease; carries D-026, D-028, D-035 | HOLD, three KEEPALIVEs, RELEASE; then lease expiry with no keepalives |
| 11 | `command` | last, because everything it dispatches to has moved | representative commands in `HELP`; assert each expected result, including refusals/errors, without resetting Experiment 3 or clearing storage |

**Stages 6 and 9 are the two that carry real risk.** Stage 6 moves the RTC
globals, where a header-defined copy silently resets the session clock. Stage 9
inverts the command pump, and its hardware check has to include the cold-boot
maintenance window specifically: that window is the reason the edge exists
(D-021), and it is the one path that cannot be exercised from an ordinary
rendezvous.

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

**Stages 6, 7, 9 and 10 need more than this.** They move autonomous state,
experiment accounting, the orchestrator and the host session. Run the full
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
  two modules.** Ten are `rtcAuto*` and belong to `auto_state`;
  `rtcSleepTestMagic` and `rtcSleepTestCycle` belong to `power_test` (D-011).
  Counted by `grep -c RTC_DATA_ATTR` on 2026-09-17. Moving all twelve into
  `auto_state` would put the deep-sleep power test's cycle counter under the
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
**226 passed, 1 xfailed**, still the same xfail. The full local gate, including
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

### DIAG_ALRT read order may lose charge-overflow evidence — source question, 2026-09-23

`runAutonomousWakeCycle()` reads CHARGE and ENERGY before `DIAG_ALRT`.
Its comment and STORAGE_SYNC_DESIGN say `CHARGEOF` clears when CHARGE is read.
If that stated clear-on-read behavior is correct, reading the flag afterwards,
even in the same wake, cannot establish that no charge overflow occurred.
This audit confirms the ordering contradiction only; it has not verified the
silicon behavior against the datasheet or induced an overflow on hardware.

**Open question:** should DIAG_ALRT be captured before the accumulator reads?
Verify the datasheet's clear conditions and exercise an overflow before changing
runtime order. Until then, do not treat same-wake sampling as validated overflow
protection. This is separate from the existing scheduler-overrun `xfail`.

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

## Wi-Fi NTP and Bluetooth time sync

The `SET_TIME` operation is designed to be transport-independent, so NTP over Wi-Fi and a Bluetooth mobile client both feed the same logical operation with no change to record semantics. Neither transport is implemented.

The first one worth building is automatic `SET_TIME` from the Python logger on USB connect, which is listed in the storage design rather than here because it is part of that milestone.

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

## Explicit maintenance mode for an autonomously armed device

The 15-second cold-boot maintenance window ([DECISIONS.md](DECISIONS.md) D-021) is a development recovery mechanism. It costs awake time on every cold boot and it requires physical access to reset the board.

Two better mechanisms are worth considering once the logger leaves the bench:

- **A hardware maintenance signal.** A button or a jumper read at boot, so a deployed logger can be put into management mode deliberately rather than by timing a window. This is the more robust option, and it costs a pin and a part.
- **A BLE management mode.** Would allow stopping, inspecting, and syncing without physical access, and shares the transport that time sync and record sync will want anyway. It costs radio power and a larger attack surface on a device parked in a car.

USB-host detection was investigated as a way to open the window only when someone is actually attached, and it is not reliable enough to gate recovery on. See [LAB_NOTES.md](LAB_NOTES.md) 2026-09-11 for what `isPlugged()` and `isConnected()` actually report. The diagnostics are printed on every window open and close, so the data to revisit this is being collected.

Not scheduled.

## Battery state of charge

The application does not display or claim to measure battery state of charge, and the cumulative-charge chart is charge through the INA228-measured solar path only. Future SOC work should consider BMW IBS data and/or a rested-voltage model. See [DECISIONS.md](DECISIONS.md) D-009.
