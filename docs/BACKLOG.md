# Backlog

Work and ideas that are not part of the current implementation. Items here are not decisions; they become entries in [DECISIONS.md](DECISIONS.md) when they are settled and implemented.

## How this file is organized

- **Current and near-term work** — committed, expected to happen next.
- **Known bugs and issues** — real defects, diagnosed, not yet fixed.
- **Experiments to run** — bench measurements that are defined but not yet taken.
- **Longer-term ideas** — promising concepts that are **not** committed work and must not be read as planned.

The boundary that matters most is the last one. Everything under "Longer-term ideas" is speculation with a rationale attached, not a roadmap.

---

# Current and near-term work

Nothing is currently committed here.

Recently completed and moved out of this file:

- **INA228 shutdown mode across deep sleep.** Implemented as `POWER TEST SLEEP INA OFF` and measured on 2026-09-10: 0.33 mA against the 1.07 mA INA-continuous baseline, a 0.74 mA saving. See [LAB_NOTES.md](LAB_NOTES.md) and [PROJECT.md](PROJECT.md).

---

# Known bugs and issues

## Host watchdog is not aware of power-test telemetry cadence

During `POWER TEST SLEEP` the firmware intentionally suspends `CSV_SAMPLE` and `CSV_DATA` and emits only the 5-second heartbeat. The Python logger's silence watchdog warns after 3 seconds (`SILENT_WARNING_SECONDS` in `app/solar_logger.py`), so every gap between heartbeats trips it.

The observed result during the 2026-09-10 measurement was a repeating pair on every awake phase:

```text
[SERIAL] State: CONNECTED-BUT-SILENT
[SERIAL] Firmware data resumed: ALL OK
```

This is a diagnostic-state bug, not a measurement failure and not a serial fault. Nothing was lost. The 10-second `SILENT_DISCONNECT_SECONDS` threshold is never reached, so the port is never actually dropped over it; the warnings are noise that trains the operator to ignore a watchdog that exists to catch a real ROM-downloader or dead-sketch condition.

Note that the disconnect during the 30-second deep sleep itself is genuine — the USB device really does disappear. Only the in-awake-phase warnings are false.

Fix later by making the watchdog aware of intentional power-test telemetry cadence and state, so its expectations track what the firmware is actually supposed to be sending. Not scheduled.

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

## Hardware power-gating of the INA228

Cutting INA228 supply rather than using its shutdown mode. The 2026-09-10 measurement makes this much less attractive than it looked beforehand: shutdown already recovered 0.74 mA of the 1.07 mA, and the INA228's own shutdown draw is specified at 2.8 µA typical, so gating the rail can recover at most a few microamps more.

It also costs a board change, and it loses the property that shutdown is a reversible register write with no external parts.

## Store-and-forward telemetry logger

### Current system

- Detailed sample and interval history exists only when the host Python logger is connected and writing CSV.
- The ESP32 persists experiment and accounting state to NVS, but not full telemetry history.
- Serial output produced while the host is absent is lost.

This is visible today in two places already recorded elsewhere. `CSV_EVENT` messages emitted during `setup()` are normally lost, because the host has not finished rediscovering the re-enumerated USB device yet; that affects `EXPERIMENT_RESUME` and, during the deep-sleep power test, `SLEEP_TEST_WAKE`. And the deep-sleep power test itself produces no durable telemetry at all while it runs, which is exactly the condition the board will be in for any real unattended deployment.

### Future direction

- The ESP32 should persist timestamp or sequence-based telemetry locally.
- A later host, Bluetooth, or Wi-Fi connection should sync the unsent records.
- The host should acknowledge received records.
- Storage reclamation should be a separate operation from experiment reset. Freeing space must not be reachable only by destroying experiment state.
- NVS is likely appropriate for state and checkpoints, not for a full long-term time series.
- Investigate LittleFS or another flash-backed append-only format.

Not scheduled. Nothing in this section is implemented.

## Battery state of charge

The application does not display or claim to measure battery state of charge, and the cumulative-charge chart is charge through the INA228-measured solar path only. Future SOC work should consider BMW IBS data and/or a rested-voltage model. See [DECISIONS.md](DECISIONS.md) D-009.
