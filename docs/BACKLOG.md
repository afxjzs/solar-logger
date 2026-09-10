# Backlog

Design direction that is agreed but deliberately not built yet. Items here are not decisions; they become entries in [DECISIONS.md](DECISIONS.md) when they are settled and implemented.

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

## INA228 shutdown mode across deep sleep — IMPLEMENTED, measurement pending

This item is built. `POWER TEST SLEEP INA OFF` is in the firmware and compiles; it has not been run on hardware. See [PROJECT.md](PROJECT.md) and [LAB_NOTES.md](LAB_NOTES.md).

Remaining work is the bench run itself: measure the deep-sleep steady current with the INA228 in shutdown and compare against the 1.07 mA INA-continuous baseline.

The datasheet predicts a saving of roughly 640 µA (IQ 640 µA typical against IQSD 2.8 µA typical). That is arithmetic, not a result.

**Hardware power-gating remains the fallback, not the next step.** A register write costs nothing, needs no board changes, and is reversible; gating the rail is a bigger commitment and should be justified by what this measurement shows.

One question the datasheet already answered while this was being built: the 60 µs start-up time from shutdown is four orders of magnitude below the existing 2-second ADC settle, so the wake path needed no new delay. The other question — whether shutdown loses CHARGE and ENERGY accumulation — is answered yes, and it is the central tradeoff rather than a side effect. Interval accounting is already suspended during either sleep-test variant, so it does not affect the test; it matters a great deal for any future duty-cycled logger, which would probably want the INA-continuous variant precisely so the hardware accumulators keep integrating through the sleep window.

## Power characterization still open

- Deep-sleep wake transient. The DMM briefly displayed overload during some wake transitions on 2026-09-10, so peak current is uncharacterized. Needs an instrument faster than a handheld meter.
- Wi-Fi transient current. The steady STA-idle delta is measured; burst behavior is not. The DMM was on its 10 mA range, which may average out short ESP32 current bursts.
- Decomposing the 1.07 mA deep-sleep figure into ESP32 and INA228 shares. The INA228 was verified powered during that measurement, but its share of the total is unknown. The shutdown-mode variant above is the first attempt at this; hardware power-gating is the fallback.
- A trustworthy figure for INA228 consumption in the awake logger. The old `28.4 mA - 18.6 mA = 9.8 mA` subtraction is marked NOT VALID in [LAB_NOTES.md](LAB_NOTES.md), because removing the INA228 also changes firmware behavior into a halted FATAL loop. The datasheet says 640 µA typical. Getting a measured number needs two states that differ only in the INA228, which the shutdown mode now makes possible while the logger keeps running normally.
