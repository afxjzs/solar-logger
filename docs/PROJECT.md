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

## Firmware Power Test

The firmware supports a persisted, autonomous Wi-Fi test:

```text
POWER TEST WIFI
POWER TEST STATUS
POWER TEST STOP
```

The test repeats these non-blocking phases:

```text
WIFI OFF for 30 seconds
WIFI ON in station mode, not associated with an access point, for 30 seconds
WIFI OFF for 30 seconds
```

The armed flag is stored in a separate NVS key from experiment state. It survives USB disconnect, loss of power, external AA battery power, and reboot. The test changes only Wi-Fi state; it does not reset or increment the experiment or alter INA228 measurement behavior.
