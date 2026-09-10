# Lab Notes

## Bench setup

The measured system is on a desk, not in a car. A jump-and-carry jump pack's battery, removed from its case, stands in for the BMW 12 V battery. The solar panel is in a window.

Every measurement in this file was taken against that arrangement. Irradiance is whatever the window gave at that time of day, so absolute solar numbers are not comparable across sessions and are not a model of the panel on a car. Current draw measurements of the XIAO and INA228 themselves are unaffected by this.

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
