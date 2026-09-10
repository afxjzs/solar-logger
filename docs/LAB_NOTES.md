# Lab Notes

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

Treat 9.8 mA only as an approximate INA228 breakout/subsystem contribution.

An earlier test disconnected only the INA228 3V3 supply and changed the reading by about 0.04 mA. That result is invalid for estimating INA228 power because SDA, SCL, and GND remained attached and could back-power the INA228 through its signal pins. The complete electrical disconnect is the useful comparison.

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

