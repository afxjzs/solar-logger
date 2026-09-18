# BMW Solar Logger Documentation

These documents are the canonical engineering record for the BMW Solar Logger. Update them in the same work as substantive code changes, measurements, corrections, architecture decisions, and open questions. Do not rely on chat history as the authoritative record.

## Documents

- [PROJECT.md](PROJECT.md) - current repository layout, workflow, telemetry, and operating procedures.
- [DECISIONS.md](DECISIONS.md) - architecture decisions and constraints that should remain stable.
- [LAB_NOTES.md](LAB_NOTES.md) - dated bench results, test setup, qualifications, and follow-up work.
- [BACKLOG.md](BACKLOG.md) - agreed design direction that is deliberately not built yet, including the store-and-forward telemetry logger.
- [STORAGE_SYNC_DESIGN.md](STORAGE_SYNC_DESIGN.md) - design for the autonomous local logging and later synchronization milestone, plus Section 12a describing the bench prototype that implements one cycle of it. Confirmed findings are marked separately from proposals.

## Current Project

All paths below are relative to the repository root.

- Firmware: `Arduino/solar-logger/solar-logger.ino`
- INA228 driver module: `Arduino/solar-logger/ina228.h`, `Arduino/solar-logger/ina228.cpp`
- Python logger: `app/solar_logger.py`
- Current telemetry: `data/samples.csv`, `data/intervals.csv`, `data/events.csv`
- Legacy/archive data: `logs/`
- Firmware identity: `Arduino/solar-logger/firmware_version.h`
- Canonical upload tool: `tools/upload.sh`
- Serial command tool: `tools/send.sh`
- Local validation gate: `tools/check.sh` (tests, type check, warning-free compile; never uploads)
- CSV archiver: `tools/archive-data.py` (host files only; never touches the device)
- Editor IntelliSense config: `tools/intellisense.sh` (`--check` reports staleness)

The firmware reports its own identity at boot, on every autonomous wake, in `STATUS`, and on the `VERSION` command: a hand-edited version, the Git revision injected by `tools/upload.sh`, and the command-protocol build ID. An image built any other way prints `Revision: UNKNOWN` rather than a blank. See [DECISIONS.md](DECISIONS.md) D-038 and the version policy in [PROJECT.md](PROJECT.md).

Firmware modularization has started. The INA228 driver became its own module on 2026-09-18; it is compiled and host-tested and has **not** yet been validated on hardware. Every extraction stage is gated on `tools/check.sh` and the characterization tests that freeze the current firmware's behavior, then on a hardware smoke test. See [DECISIONS.md](DECISIONS.md) D-043 and D-047, and the extraction plan in [BACKLOG.md](BACKLOG.md).

`tools/send.sh` waits for the next USB rendezvous by default, because a sleeping board's absent USB device is the ordinary state. `LOGGER SESSION KEEPALIVE` and `LOGGER SESSION RELEASE` are excluded: they address a session that already exists, so they fail immediately instead of acting on a later one. See [DECISIONS.md](DECISIONS.md) D-040.

The firmware supports persisted bench power tests: `POWER TEST WIFI`, and the deep-sleep test in two variants, `POWER TEST SLEEP` and `POWER TEST SLEEP INA OFF`. Only one may be armed at a time. The deep-sleep test suspends interval accounting and machine-readable CSV output for its duration; the reasoning and limitations are recorded in [PROJECT.md](PROJECT.md) and [DECISIONS.md](DECISIONS.md) D-011 through D-016.

Both deep-sleep variants were measured on 2026-09-10, against approximately 28.2-28.6 mA awake:

| Deep sleep, INA228 continuous | Deep sleep, INA228 shutdown |
| ---: | ---: |
| 1.07 mA | 0.33 mA |

The autonomous logger bench prototype (`LOGGER TEST AUTONOMOUS`) ran on hardware on 2026-09-11 and has produced 65 durable 72-byte records, a 4680-byte log, and an intact tail, with hardware-accumulated charge and energy surviving deep sleep. Records before sequence 188 carry `exp=0` from a known prototype bug rather than a real experiment 0. **No wake timings have been measured yet**; that run's instrumentation was reporting the wrong span and has been rewritten. An armed board is stopped by resetting it and sending `LOGGER TEST STOP` during the 15-second cold-boot maintenance window. See [DECISIONS.md](DECISIONS.md) D-021 through D-023.

The INA228 was verified powered throughout the continuous run by probing the 3V3 rail directly across multiple cycles. **Keeping the INA228 converting through the ESP32's sleep costs roughly 0.74 mA and buys continuous hardware CHARGE and ENERGY accumulation; shutting both down reaches 0.33 mA with measurement and accumulation entirely suspended.** Neither is simply better.

One earlier inference has been retracted: the `28.4 mA - 18.6 mA = 9.8 mA` estimate of INA228 consumption is marked NOT VALID in [LAB_NOTES.md](LAB_NOTES.md), because removing the INA228 also halts the firmware into its FATAL loop, so the two readings came from different firmware states. The datasheet figure is 640 µA typical.

The live plot uses host local time, a rolling recent-history window, a fourth chart for cumulative solar charge, interactive hover inspection, and visible Home/−/+/Follow controls. Plot lines and hover cursors persist across updates for responsive interaction. Auto-follow is the default; use `+` and `−` to change the visible time window, then toggle `Follow` or press `f` to control whether that zoomed window tracks the latest data. Plotting changes and remaining visualization work are recorded in [LAB_NOTES.md](LAB_NOTES.md).
