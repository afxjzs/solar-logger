# BMW Solar Logger

An instrumented bench rig that measures a solar charging path with a TI INA228, streams telemetry to a Mac over USB serial, and plots it live.

The measured system is a solar panel maintaining a 12 V lead-acid battery. On the bench that battery is a jump pack's cell sitting on a desk, with the panel in a window.

## Hardware

- Seeed Studio XIAO ESP32-C3
- TI INA228 current/voltage/power monitor at I2C address `0x40`, on `D4` (SDA) and `D5` (SCL)
- Shunt calibrated to **15.62 mΩ**, not the 15 mΩ nominally printed on the breakout — the difference came out of DMM measurement and matters for every current reading

## What it does

The firmware stays awake and highly observable. It emits three kinds of machine-readable line over serial, alongside human-readable diagnostics:

| Message | Cadence | Contents |
| --- | --- | --- |
| `CSV_SAMPLE` | 1/second | instantaneous voltage, current, power, temperature |
| `CSV_DATA` | 1/minute | completed interval using the INA228's **hardware** CHARGE and ENERGY accumulators |
| `CSV_EVENT` | on change | experiment lifecycle and power-test markers |

Experiment state — id, interval number, running charge and energy — is checkpointed to ESP32 NVS every interval, so it survives power loss.

The Python host logger discovers the board, prints everything the firmware says, adds a timezone-aware host timestamp to every row, writes durable CSVs, and drives a four-panel live plot with zoom, follow, and hover inspection.

## Power characterization results

The rig was used to characterize its own consumption. All figures are DMM readings with the board on external AA power and USB physically disconnected.

| State | Current |
| --- | ---: |
| Awake, Wi-Fi off | ~28.2–28.6 mA |
| Awake, Wi-Fi STA idle (not associated) | ~30.5 mA |
| ESP32 deep sleep, INA228 still converting | 1.07 mA |
| ESP32 deep sleep, INA228 in shutdown | 0.33 mA |

The headline tradeoff: **keeping the INA228 converting through the ESP32's sleep costs about 0.74 mA and preserves continuous hardware charge accumulation. Shutting both down reaches 0.33 mA, with measurement and accumulation entirely suspended.** Neither is simply better.

Full conditions, qualifications, and one retracted inference are in [docs/LAB_NOTES.md](docs/LAB_NOTES.md).

## Getting started

Requires [`uv`](https://docs.astral.sh/uv/) and [`arduino-cli`](https://arduino.github.io/arduino-cli/) with the `esp32:esp32` core installed.

```bash
# Run the host logger: live plot plus durable CSV capture
uv run python app/solar_logger.py

# Compile and flash the firmware
tools/upload.sh

# Send a command to the board
tools/send.sh STATUS
```

`tools/upload.sh` and `tools/send.sh` are the supported paths, not conveniences. Three processes want the same USB device, and these coordinate ownership through two small file protocols so the logger, the uploader, and the command sender can never race for the port. Uploading with `arduino-cli` directly while the logger is running will fight it for the device.

## Serial commands

```text
HELP
STATUS
WIFI ON | WIFI OFF | WIFI STATUS
POWER TEST WIFI              repeating Wi-Fi on/off power test
POWER TEST SLEEP             deep-sleep test, INA228 keeps converting
POWER TEST SLEEP INA OFF     deep-sleep test, INA228 enters shutdown first
POWER TEST STATUS
POWER TEST STOP
RESET YES                    start a NEW experiment (destructive to the current one)
```

The power tests persist their armed state to NVS, so you can arm one over USB, unplug, and run the whole test on battery with a meter in series.

## Repository layout

```text
Arduino/solar-logger/    current firmware
Arduino/*/               earlier bench sketches, kept as a development record
app/solar_logger.py      host logger and live plot
tools/                   upload and serial-command scripts
docs/                    the engineering record — start at docs/INDEX.md
data/                    live telemetry (gitignored)
logs/                    archived early telemetry
```

## Documentation

[docs/INDEX.md](docs/INDEX.md) is the entry point. The docs are the project's engineering record rather than a tidied-up summary, and they deliberately keep the mistakes:

- [PROJECT.md](docs/PROJECT.md) — how the system works and how to operate it
- [DECISIONS.md](docs/DECISIONS.md) — architecture decisions and the reasoning behind them
- [LAB_NOTES.md](docs/LAB_NOTES.md) — dated bench results, with qualifications and corrections
- [BACKLOG.md](docs/BACKLOG.md) — known issues, experiments to run, and longer-term ideas

Two things worth knowing if you read the results: the bench setup is a desk and a window, so absolute solar figures are not comparable across sessions, and wake-transient peak current is uncharacterized because a handheld DMM overloads on it.

## License

No license is currently specified.
