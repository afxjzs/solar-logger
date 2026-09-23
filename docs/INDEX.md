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
- Connection module (which transport a host has claimed): `Arduino/solar-logger/connection.h`, `Arduino/solar-logger/connection.cpp`
- NVS persistence module (the namespace, the keys, the typed reads and writes): `Arduino/solar-logger/nvs_persistence.h`, `Arduino/solar-logger/nvs_persistence.cpp`
- Telemetry module (how each machine-readable CSV line is spelled): `Arduino/solar-logger/telemetry.h`, `Arduino/solar-logger/telemetry.cpp`
- Python logger: `app/solar_logger.py`
- Current telemetry: `data/samples.csv`, `data/intervals.csv`, `data/events.csv`
- Legacy/archive data: `logs/`
- Firmware identity: `Arduino/solar-logger/firmware_version.h`
- Canonical upload tool: `tools/upload.sh`
- Serial command tool: `tools/send.sh`
- Local validation gate: `tools/check.sh` (tests, type check, warning-free compile, editor database; never uploads)
- CSV archiver: `tools/archive-data.py` (host files only; never touches the device)
- Editor IntelliSense config: `tools/intellisense.sh` (finds every module itself; `tools/check.sh` runs it; `--check` reports staleness)

Current source and the latest supplied hardware transcript report `0.3.0-dev` / `solar-logger-protocol-ack-v3`; the observed board revision was `d017f7d`. The firmware reports its own identity at boot, on every autonomous wake, in `STATUS`, and on the `VERSION` command: a hand-edited version, the Git revision injected by `tools/upload.sh`, and the command-protocol build ID. An image built any other way prints `Revision: UNKNOWN` rather than a blank. See [DECISIONS.md](DECISIONS.md) D-038 and the version policy in [PROJECT.md](PROJECT.md).

Firmware modularization has started. Four modules exist: the INA228 driver and connection bookkeeping, extracted on 2026-09-18, and NVS persistence and telemetry, extracted on 2026-09-23. The INA228 driver's hardware smoke test was reported on 2026-09-18 and is recorded, as reported, in [LAB_NOTES.md](LAB_NOTES.md). Connection bookkeeping and NVS persistence are compiled and host-tested; supplied hardware transcripts now confirm the connection smoke test on `8776ba0` and NVS restore/write smoke tests on `d017f7d`. The 2026-09-23 LAB_NOTES addendum records the evidence and its limits. Telemetry, which owns how a CSV line is spelled and never when one is emitted, is compiled and host-tested and has **not** run on hardware. Every extraction stage is gated on `tools/check.sh` and the characterization tests that freeze the current firmware's behavior, then on a hardware smoke test. A new module needs no editor configuration: the gate regenerates the IntelliSense database, which finds every module itself — `nvs_persistence.cpp` was the first module created after that rule and needed no tooling change, and `telemetry.cpp` was the second. See [DECISIONS.md](DECISIONS.md) D-043, D-047, D-048, D-049, D-050 and D-051, and the extraction plan in [BACKLOG.md](BACKLOG.md).

`tools/send.sh` waits for the next USB rendezvous by default, because a sleeping board's absent USB device is the ordinary state. `LOGGER SESSION KEEPALIVE` and `LOGGER SESSION RELEASE` are excluded: they address a session that already exists, so they fail immediately instead of acting on a later one. See [DECISIONS.md](DECISIONS.md) D-040.

The firmware supports persisted bench power tests: `POWER TEST WIFI`, and the deep-sleep test in two variants, `POWER TEST SLEEP` and `POWER TEST SLEEP INA OFF`. Only one may be armed at a time. The deep-sleep test suspends interval accounting and machine-readable CSV output for its duration; the reasoning and limitations are recorded in [PROJECT.md](PROJECT.md) and [DECISIONS.md](DECISIONS.md) D-011 through D-016.

Both deep-sleep variants were measured on 2026-09-10, against approximately 28.2-28.6 mA awake:

| Deep sleep, INA228 continuous | Deep sleep, INA228 shutdown |
| ---: | ---: |
| 1.07 mA | 0.33 mA |

Autonomous logging is a persistent mode (`LOGGER AUTONOMOUS ON/OFF/STATUS`, D-031). The 65-record inspection was an early 2026-09-11 milestone; the 2026-09-23 `d017f7d` snapshot shows 3,272 valid 72-byte records, sequences 1412–4683, zero trailing bytes, and an intact tail. Experiment 3 survived upload and RELEASE saved interval 53. These are historical observations, not live board readings. Early records before sequence 188 carried `exp=0` from a prototype bug. Claimed-wake work timings are recorded, but the full unattended wake budget and RTC drift remain uncharacterized. Stop an armed board with `LOGGER AUTONOMOUS OFF` during a rendezvous or the 15-second cold-boot maintenance window. Sync, storage ACKs, reclamation, and a storage-full policy remain unimplemented.

The INA228 was verified powered throughout the continuous run by probing the 3V3 rail directly across multiple cycles. **Keeping the INA228 converting through the ESP32's sleep costs roughly 0.74 mA and buys continuous hardware CHARGE and ENERGY accumulation; shutting both down reaches 0.33 mA with measurement and accumulation entirely suspended.** Neither is simply better.

One earlier inference has been retracted: the `28.4 mA - 18.6 mA = 9.8 mA` estimate of INA228 consumption is marked NOT VALID in [LAB_NOTES.md](LAB_NOTES.md), because removing the INA228 also halts the firmware into its FATAL loop, so the two readings came from different firmware states. The datasheet figure is 640 µA typical.

The live plot uses host local time, a rolling recent-history window, a fourth chart for cumulative solar charge, interactive hover inspection, and visible Home/−/+/Follow controls. Plot lines and hover cursors persist across updates for responsive interaction. Auto-follow is the default; use `+` and `−` to change the visible time window, then toggle `Follow` or press `f` to control whether that zoomed window tracks the latest data. Plotting changes and remaining visualization work are recorded in [LAB_NOTES.md](LAB_NOTES.md).
