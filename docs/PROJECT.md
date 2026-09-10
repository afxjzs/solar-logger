# Project

## Purpose

The BMW Solar Logger measures an INA228-powered solar measurement path with an XIAO ESP32-C3 and records live firmware output on a Mac.

## Repository

Canonical repository:

```text
/Users/afxjzs/dev/projects/solar-charger
```

| Area | Path |
| --- | --- |
| Firmware | `Arduino/solar-logger/solar-logger.ino` |
| Python logger | `app/solar_logger.py` |
| Current sample telemetry | `data/samples.csv` |
| Current interval telemetry | `data/intervals.csv` |
| Current firmware events | `data/events.csv` |
| Legacy/archive data | `logs/` |
| Firmware upload | `tools/upload.sh` |
| Firmware command sender | `tools/send.sh` |

`data/` is the current telemetry directory. `logs/` is archive/legacy data and is not the live polling destination.

## Development Workflow

Run the Python logger when live telemetry, plotting, and durable CSV capture are needed:

```text
uv run python app/solar_logger.py
```

The logger uses pyserial at 115200 baud, discovers `/dev/cu.usbmodem*`, prints all firmware output, writes host-timestamped telemetry, and reconnects after serial interruption.

Use `tools/upload.sh` as the canonical compile/upload mechanism. Compilation runs before serial coordination because it does not need the USB port.

### Upload ownership

The upload marker is the repository-root file `.upload-in-progress`. Its content protocol is:

```text
REQUESTED -> RELEASED
```

When the Python logger is running, `tools/upload.sh` detects it, writes `REQUESTED`, waits for the logger to close Serial, and waits for the logger to write `RELEASED`. The uploader discovers the current modem port only after release. The logger stays disconnected while the marker remains present, then resumes discovery after the marker is removed.

This workflow has been bench-tested successfully in both cases:

1. Python logger running: the logger released Serial, wrote `RELEASED`, upload succeeded, and the logger reconnected.
2. Python logger not running: the uploader detected no logger, skipped the handshake, and uploaded directly.

If a running logger does not acknowledge `RELEASED` within five seconds, the uploader aborts. It must not fall through to esptool because that could create a two-process Serial race.

The upload script removes `.upload-in-progress` through its exit, interrupt, and termination cleanup traps.

## Serial Commands

Use `tools/send.sh` from the repository root. Arguments are joined into one newline-terminated firmware command.

```text
tools/send.sh WIFI STATUS
tools/send.sh WIFI ON
tools/send.sh WIFI OFF
tools/send.sh STATUS
tools/send.sh HELP
tools/send.sh POWER TEST WIFI
tools/send.sh POWER TEST SLEEP
tools/send.sh POWER TEST SLEEP INA OFF
tools/send.sh POWER TEST STATUS
tools/send.sh POWER TEST STOP
```

When `app/solar_logger.py` is running, `send.sh` writes `.serial-command`. The logger reads the complete command, writes it through its existing pyserial connection, flushes the output, and removes the handoff file. This prevents a second process from opening the port.

When the logger is not running, `send.sh` discovers the current `/dev/cu.usbmodem*` port and sends the command directly at 115200 baud. It refuses to send when `.upload-in-progress` exists, so command sending cannot race a firmware upload. It also refuses to overwrite an existing pending command.

## Telemetry Files

The Python logger writes these current schemas:

- `data/samples.csv`: `captured_at` followed by the firmware sample fields.
- `data/intervals.csv`: `captured_at` followed by the firmware interval fields.
- `data/events.csv`: `captured_at,event_type,experiment_id,detail`.

`captured_at` is local, timezone-aware Mac time in ISO 8601 format. Firmware `elapsed_seconds` remains useful for experiment-relative timing and is preserved separately.

The logger checks existing CSV headers before appending. Old pre-timestamp telemetry was archived rather than deleted. `events.csv` can legitimately contain only its header when no `CSV_EVENT` message was observed while the Python logger was attached. Startup events emitted before host reconnect are not currently durable; this is a future architecture issue, not a current parser failure.

## Live Plot

The live plot uses the same timezone-aware `captured_at` value written to `samples.csv` as its x-coordinate. The x-axis displays local clock labels such as `14:31:10`; firmware elapsed time remains available in the CSV for experiment-relative analysis.

The y-axes use fixed-point formatting with Matplotlib additive offsets and scientific notation disabled, so the voltage axis shows actual values such as `13.0030` rather than a hidden `+1.3e1` offset.

At startup, the logger validates `samples.csv`, finds the latest experiment represented by valid rows, parses timezone-aware timestamps, and loads only the latest 15 minutes into the plot. Malformed historical rows produce warnings and are skipped. The same `PLOT_HISTORY_SECONDS` rolling window applies during live operation. CSV remains the complete durable history; Matplotlib holds only the display window.

A real Serial interruption adds one display-only NaN marker, which breaks all three plotted lines without creating a fake CSV row. A new experiment still clears the plotted history. A logger restart is not treated as a new experiment.

The figure does not display or claim to measure battery state of charge. Future battery-SOC work may use BMW IBS data and/or a rested-voltage model.

The fourth chart shows `Accumulated charge (mAh)` from `running_charge_mAh` in the 60-second interval accounting records. It represents charge delivered through the INA228-measured solar path, not battery state of charge. Recent interval history is reloaded from `data/intervals.csv` at startup, and no one-second cumulative-charge samples are fabricated.

Startup prints the loaded sample and interval counts plus the historical time range. If the newest valid history is older than the display window, the logger prints a stale-history warning; new live samples then move the rolling display toward current time.

The plot includes visible `Home`, `−`, `+`, and `Follow` buttons. `+` narrows the shared time window and `−` widens it; neither changes the independent Follow state. When Follow is ON, live redraws shift the selected-width window to the newest data. When Follow is OFF, the current zoom remains stable. Toggle `Follow` or press `f` to change that state. `h` restores Follow ON; `z` and `p` mirror zoom in and zoom out. The native Matplotlib toolbar remains available when the backend exposes it. Hovering a plotted line shows its local time and value. Hover support uses the `mplcursors` dependency managed by `uv`.

The renderer creates each line and hover cursor once, then updates line data in place. This avoids clearing axes, rebuilding cursor state, and blocking with a pause every sample, which keeps the desktop UI responsive during live telemetry.

## Firmware Power Tests

The firmware supports two persisted, autonomous bench power tests. Only one may be armed at a time, and the firmware refuses to arm a second one rather than silently replacing the first. `POWER TEST STOP` stops whichever test is active, and `POWER TEST STATUS` reports both.

```text
POWER TEST WIFI
POWER TEST SLEEP
POWER TEST SLEEP INA OFF
POWER TEST STATUS
POWER TEST STOP
```

Neither test resets, increments, migrates, or rewrites the experiment checkpoint.

### Wi-Fi power test

The test repeats these non-blocking phases:

```text
WIFI OFF for 30 seconds
WIFI ON in station mode, not associated with an access point, for 30 seconds
WIFI OFF for 30 seconds
```

The armed flag is stored in the NVS key `wifi_test_armed`, separate from experiment state. It survives USB disconnect, loss of power, external AA battery power, and reboot. The test changes only Wi-Fi state; it does not alter INA228 measurement behavior, and interval accounting continues normally throughout.

### Deep-sleep power test

`POWER TEST SLEEP` measures XIAO ESP32-C3 supply current while the ESP32 is in true deep sleep and the INA228 breakout remains normally powered from XIAO 3V3. The comparison it exists to support is:

```text
normal awake logger
vs
ESP32 deep sleep with INA228 still powered
```

The test repeats:

```text
AWAKE for 30 seconds, Wi-Fi OFF, normal INA228 behavior
DEEP SLEEP for 30 seconds
```

This uses `esp_sleep_enable_timer_wakeup()` and `esp_deep_sleep_start()`. It is real deep sleep, not `delay()` and not light sleep.

**Deep sleep wakes through reset semantics.** `esp_deep_sleep_start()` does not return. The chip reboots and `setup()` runs again from the top, so every wake is a fresh boot and the state machine is split across the reset boundary: `setup()` decides whether an awake phase is starting and why, and `loop()` decides when the awake phase has lasted long enough to sleep again.

The armed flag is stored in the NVS key `sleep_test_arm`, so the test survives USB disconnect and external power cycling. The per-cycle counter lives in RTC-retained memory instead, guarded by a magic value. See [DECISIONS.md](DECISIONS.md) D-011.

**INA228 stays powered in this first test.** VIN, GND, SDA, and SCL all remain connected. Power-gating the INA228 is a separate future experiment; this test deliberately measures "ESP32 deep sleep + INA228 breakout still powered."

Wi-Fi is off for the entire test. The awake-phase banner reports the radio state read back from `WiFi.getMode()` rather than asserting it, and turns the radio off with an explicit error if it finds it on.

#### Wake diagnostics

Every boot prints the wake cause from `esp_sleep_get_wakeup_cause()`:

```text
[BOOT] Wake reason: POWER ON / RESET
[BOOT] Wake cause code: 0
```

or:

```text
[BOOT] Wake reason: DEEP SLEEP TIMER
[BOOT] Wake cause code: 4
```

The raw enum value is printed alongside the name so an unrecognized cause stays diagnosable.

#### Limitations while the deep-sleep test runs

These are real limitations of running a 60-second accounting interval across repeated 30-second deep sleeps, not parser or wiring faults:

- **Interval accounting is SUSPENDED.** `millis()` restarts at zero on every wake, so the 60-second interval timer never expires during a 30-second awake phase. `setup()` also clears the INA228 accumulators on every boot, so hardware accumulation from the previous awake phase is discarded. There is no honest way to report 60 seconds of elapsed interval time for a board that was asleep for half of it. Accounting is therefore gated off explicitly rather than left to fail by arithmetic accident. Experiment ID, interval number, running charge, and running energy are all preserved unchanged.
- **`CSV_SAMPLE` and `CSV_DATA` output is suspended.** `elapsed_seconds` is derived from the frozen interval number and from `millis()`, so it would sawtooth backwards on every wake while `experiment_id` stayed the same. The human-readable heartbeat keeps printing real INA228 values every five seconds.
- **`CSV_EVENT,EXPERIMENT_RESUME` is emitted on every wake,** because the board really did reboot. During the test that is roughly one event per cycle when a host is attached.
- **Each wake costs about 3.5 seconds of extra awake time** before the 30-second awake phase begins: `delay(1500)` for USB re-enumeration plus `delay(2000)` for the ADC to settle. The awake-phase banner prints the measured boot-to-awake-phase time, so the real duty cycle is observable rather than assumed.
- **`RESET YES` is refused** while accounting is suspended. A new experiment created in that state would be frozen from birth and its first interval would never close.

`POWER TEST STOP` clears the persisted flag, ensures Wi-Fi is off, clears the INA228 accumulators, restarts the interval timer, and resumes normal continuous logging. If the accumulators cannot be cleared, accounting deliberately stays suspended and says so, because folding an unaccounted window of unknown length into the next interval would produce a wrong average current and average power that look exactly like right ones.

#### Reproduction procedure

1. Connect the INA228 normally and power the XIAO through VUSB from the 3xAA holder, with the DMM in series.
2. Keep USB connected only long enough to send `POWER TEST SLEEP`.
3. Confirm with `POWER TEST STATUS` that the sleep test is armed.
4. Disconnect USB physically.
5. Apply external battery power and let the board reboot.
6. Watch the DMM alternate between the awake reading and the deep-sleep reading on a roughly 33.5 s / 30 s cycle.
7. Reconnect USB and send `POWER TEST STOP` during an awake phase. The board cannot receive Serial commands while it is asleep.

#### Measured result

Run on 2026-09-10. Full conditions and qualifications are in [LAB_NOTES.md](LAB_NOTES.md).

| Phase | Displayed current |
| --- | ---: |
| Initial boot | approximately 18.8-19.3 mA |
| Normal awake steady state | approximately 28.2-28.6 mA |
| Deep sleep steady state | 1.07 mA |

That is approximately 96.2% lower steady current in deep sleep, roughly a 26x reduction against the ~28.4 mA awake baseline. The sleep reading repeated across multiple cycles with essentially identical values.

**The INA228 was verified as powered throughout.** The XIAO 3V3 / INA228 VIN rail was probed directly across multiple awake and deep-sleep cycles and held steady at approximately 3.28-3.29 V for several minutes, with the INA228 physically attached the whole time. So 1.07 mA is the measured steady supply current of the XIAO ESP32-C3 in true deep sleep with the INA228 breakout still powered.

One thing this measurement does NOT establish: **the wake transient.** The DMM briefly displayed overload during some wake transitions. No peak-current value was captured, and none should be assumed or back-calculated.

It also does not decompose the 1.07 mA into ESP32 and INA228 shares. The rail measurement proves the part was powered, not how much of the total it accounts for.

### Deep-sleep power test, INA-OFF variant

`POWER TEST SLEEP INA OFF` is the same test with one difference: the INA228 is placed in its own shutdown mode immediately before each ESP32 deep sleep. It exists to measure the lowest power reachable **without** physically power-gating the INA228.

Both variants remain available and only one can be armed at a time. Arming the other variant while one is armed is refused; stop first, then arm the one you want. The variant is persisted in the NVS key `sleep_ina_off` alongside the armed flag, so it survives external power cycling — a variant that reset to the default on power-up would run the wrong experiment while `POWER TEST STATUS` reported the right one.

Neither variant removes INA228 power, touches the XIAO 3V3 rail, or disconnects SDA/SCL.

#### How shutdown is entered and verified

Through the MODE bits of `ADC_CONFIG`, per TI datasheet SLYS021A Table 7-6: register address `1h`, reset `FB68h`, MODE in bits 15-12, then VBUSCT, VSHCT, VTCT, and AVG. The datasheet documents two shutdown encodings, `0h` and `8h`; the firmware writes `0h` and accepts either on readback.

Before each sleep the firmware prints the register, writes only the MODE field, reads it back, and verifies:

```text
[POWER TEST] INA-OFF sleep test
[INA228] Entering shutdown mode...
[INA228] ADC_CONFIG before: 0xFB6B
[INA228] ADC_CONFIG after:  0x0B6B
[INA228] Shutdown mode verified: ALL OK
[POWER TEST] Entering ESP32 DEEP SLEEP for 30 seconds...
```

Verification covers two conditions, not one: the MODE bits must read back as a documented shutdown value, **and** the non-MODE bits must be unchanged. A shutdown write that also moved the conversion times or averaging would mean the next wake restored something other than the known-good configuration.

If either check fails, the firmware prints an explicit error, does **not** enter deep sleep, and stops the test. A sleep current measured with the INA228 in an unknown state would be worthless, so refusing to sleep is the correct outcome.

#### Measurement semantics

While the INA228 is shut down, no ADC conversions run. Voltage, current, power, and temperature registers stop updating, and hardware CHARGE and ENERGY accumulation stops. The datasheet notes that registers remain readable in shutdown, which is exactly why this matters: the values are stale, not new.

The firmware refuses to present them as live. While shutdown is active, `STATUS` and the heartbeat both report that the INA228 is shut down and print no measurements rather than printing old ones.

**This variant deliberately sacrifices solar measurement during the sleep phase.** That is its purpose, not a defect.

#### Wake behavior

Deep sleep wakes by reboot, so `setup()` runs its normal path: `configureIna228()` rewrites CONFIG, ADC_CONFIG, and SHUNT_CAL and verifies all three by readback, and the existing 2-second ADC settle is retained. The datasheet's start-up time from shutdown is 60 µs, four orders of magnitude shorter, so no new delay was needed.

On top of that, the INA-OFF path re-reads `ADC_CONFIG` and confirms continuous conversion from the device before saying so:

```text
[BOOT] Wake reason: DEEP SLEEP TIMER
[POWER TEST] Woke from INA-OFF deep sleep: ALL OK
[INA228] Continuous measurement restored: ALL OK
```

If the device does not report continuous mode despite boot configuration reporting success, the firmware reconfigures, re-verifies, and stops the test if it still cannot confirm.

#### Architectural tradeoff

| | ESP asleep + INA continuous | ESP asleep + INA shutdown |
| --- | --- | --- |
| Hardware charge/energy accumulation | preserved | lost during sleep |
| High-resolution samples during sleep | lost | lost |
| Solar measurement during sleep | preserved | sacrificed |
| Measured supply current | 1.07 mA | pending |

The INA-continuous variant is the one a real duty-cycled logger would probably want, because the INA228's hardware accumulators keep integrating charge through the sleep window even though the ESP32 is not sampling. The INA-OFF variant tells us what that preservation costs.

#### Expected saving, from the datasheet

INA228 IQ is specified at 640 µA typical and 750 µA maximum; IQSD in shutdown is 2.8 µA typical and 5 µA maximum. Arithmetic suggests shutdown should save roughly 640 µA against the 1.07 mA baseline. **That is a datasheet prediction, not a measurement.** Record what the DMM shows.

#### Next measurement goal

Run `POWER TEST SLEEP INA OFF` on hardware and compare against the 1.07 mA INA-continuous baseline. Hardware power-gating stays a fallback and should only be considered once this result is in.
