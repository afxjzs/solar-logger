# BMW Solar Logger Documentation

These documents are the canonical engineering record for the BMW Solar Logger. Update them in the same work as substantive code changes, measurements, corrections, architecture decisions, and open questions. Do not rely on chat history as the authoritative record.

## Documents

- [PROJECT.md](PROJECT.md) - current repository layout, workflow, telemetry, and operating procedures.
- [HARDWARE_WIRING.md](HARDWARE_WIRING.md) - current USB-powered bench logic/I2C wiring and Mermaid diagram; open measurement-path topology and future vehicle-power boundary.
- [DECISIONS.md](DECISIONS.md) - architecture decisions and constraints that should remain stable.
- [LAB_NOTES.md](LAB_NOTES.md) - dated bench results, test setup, qualifications, and follow-up work.
- [BACKLOG.md](BACKLOG.md) - agreed design direction that is deliberately not built yet, including the store-and-forward telemetry logger.
- [STORAGE_SYNC_DESIGN.md](STORAGE_SYNC_DESIGN.md) - design for the autonomous local logging and later synchronization milestone, plus Section 12a describing the bench prototype that implements one cycle of it. Confirmed findings are marked separately from proposals.

## Installed direction — planned, not built

The first installed version uses the BMW's under-hood charging/jump points with
the logger in the glove box/cabin; V1 requires neither an IBS/LIN tap nor a
separate ignition wire for vehicle-on detection. A later IBS phase is expected
to move the enclosure to the trunk, with electrical topology and signal access
still OPEN. BLE connects the logger to a native iPhone app, which saves before
ACKing, supplies time and bridges to the self-hosted server. Home Wi-Fi is not
a dependency. Manual physical SYNC remains permanent; vehicle-on wake is future
research. See [PROJECT.md](PROJECT.md#installed-system-architecture--planned-not-installed-or-validated),
[DECISIONS.md](DECISIONS.md) D-052–D-055, and the installed-system work in
[BACKLOG.md](BACKLOG.md).

## Current Project

All paths below are relative to the repository root.

- Firmware: `Arduino/solar-logger/solar-logger.ino`
- INA228 driver module: `Arduino/solar-logger/ina228.h`, `Arduino/solar-logger/ina228.cpp`
- Connection module (which transport a host has claimed): `Arduino/solar-logger/connection.h`, `Arduino/solar-logger/connection.cpp`
- NVS persistence module (the namespace, the keys, the typed reads and writes): `Arduino/solar-logger/nvs_persistence.h`, `Arduino/solar-logger/nvs_persistence.cpp`
- Telemetry module (how each machine-readable CSV line is spelled): `Arduino/solar-logger/telemetry.h`, `Arduino/solar-logger/telemetry.cpp`
- Record format module (the deployed 72-byte durable record and its CRC): `Arduino/solar-logger/record_format.h`, `Arduino/solar-logger/record_format.cpp`
- Durable-log scan/recovery behavior: still in `Arduino/solar-logger/solar-logger.ino`; executed on the host by `tests/test_characterization_storage_recovery.py`
- Python logger: `app/solar_logger.py`
- Current telemetry: `data/samples.csv`, `data/intervals.csv`, `data/events.csv`
- Legacy/archive data: `logs/`
- Firmware identity: `Arduino/solar-logger/firmware_version.h`
- Canonical upload tool: `tools/upload.sh`
- Serial command tool: `tools/send.sh`
- Local validation gate: `tools/check.sh` (tests, type check, warning-free compile, editor database; never uploads)
- CSV archiver: `tools/archive-data.py` (host files only; never touches the device)
- Editor IntelliSense config: `tools/intellisense.sh` (finds every module itself; `tools/check.sh` runs it; `--check` reports staleness)

Current source and the latest supplied hardware transcript report `0.3.0-dev` / `solar-logger-protocol-ack-v3`; the last clean record-format hardware image was `eba3b5d`, and the latest supplied recovery smoke image reported `eba3b5d-dirty` (uncommitted recovery changes based on that clean commit). These are dated hardware observations, not a fresh board query. The firmware reports its own identity at boot, on every autonomous wake, in `STATUS`, and on the `VERSION` command: a hand-edited version, the Git revision injected by `tools/upload.sh`, and the command-protocol build ID. An image built any other way prints `Revision: UNKNOWN` rather than a blank. See [DECISIONS.md](DECISIONS.md) D-038 and the version policy in [PROJECT.md](PROJECT.md).

Firmware modularization has started. Five modules exist: the INA228 driver and connection bookkeeping, extracted on 2026-09-18; NVS persistence and telemetry, extracted on 2026-09-23; and record format, extracted on 2026-09-24. The INA228 driver's hardware smoke test was reported on 2026-09-18 and is recorded, as reported, in [LAB_NOTES.md](LAB_NOTES.md). Connection bookkeeping and NVS persistence are compiled and host-tested; supplied hardware transcripts now confirm the connection smoke test on `8776ba0` and NVS restore/write smoke tests on `d017f7d`. The 2026-09-23 LAB_NOTES addendum records the evidence and its limits. Telemetry is compiled and host-tested and has now run in the smoke-validated `eba3b5d` image; the supplied evidence is not an exhaustive CSV comparison. Record format owns representation/CRC/structural validation, not creation policy, and is **extracted, software-tested and hardware-validated on clean `eba3b5d`**. Every extraction stage is gated on `tools/check.sh` and the characterization tests that freeze the current firmware's behavior, then on a hardware smoke test. A new module needs no editor configuration: the gate regenerates the IntelliSense database, which finds every module itself — `nvs_persistence.cpp` was the first module created after that rule and needed no tooling change, `telemetry.cpp` was the second and `record_format.cpp` the third. See [DECISIONS.md](DECISIONS.md) D-043, D-047, D-048, D-049, D-050, D-051 and D-056, and the extraction plan in [BACKLOG.md](BACKLOG.md).

**Latest coding-agent software gate (not rerun by this doc reconciliation): 429 passed, 1 xfailed**; pyright clean, py_compile/clean warning-free Arduino compile/shell syntax/whitespace passed, IntelliSense 6/6 automatically discovered project units. The short-cadence overrun xfail remains OPEN. The latest `eba3b5d-dirty` healthy-log smoke progressed from 4,644 records/newest 6055 to 4,649/6060; final dump 4,650 records, invalid 0, through sequence 6061, Experiment 3 intact. LittleFS extraction is the next structural candidate, not implemented. Details and evidence limits: [LAB_NOTES.md](LAB_NOTES.md#2026-09-24-hardware-validation-addendum--clean-record-format-and-working-tree-recovery).

`tools/send.sh` waits for the next USB rendezvous by default, because a sleeping board's absent USB device is the ordinary state. `LOGGER SESSION KEEPALIVE` and `LOGGER SESSION RELEASE` are excluded: they address a session that already exists, so they fail immediately instead of acting on a later one. See [DECISIONS.md](DECISIONS.md) D-040.

The firmware supports persisted bench power tests: `POWER TEST WIFI`, and the deep-sleep test in two variants, `POWER TEST SLEEP` and `POWER TEST SLEEP INA OFF`. Only one may be armed at a time. The deep-sleep test suspends interval accounting and machine-readable CSV output for its duration; the reasoning and limitations are recorded in [PROJECT.md](PROJECT.md) and [DECISIONS.md](DECISIONS.md) D-011 through D-016.

Both deep-sleep variants were measured on 2026-09-10, against approximately 28.2-28.6 mA awake:

| Deep sleep, INA228 continuous | Deep sleep, INA228 shutdown |
| ---: | ---: |
| 1.07 mA | 0.33 mA |

Autonomous logging is a persistent mode (`LOGGER AUTONOMOUS ON/OFF/STATUS`, D-031). The 65-record inspection was an early 2026-09-11 milestone; the 2026-09-23 `d017f7d` snapshot shows 3,272 valid 72-byte records, sequences 1412–4683, zero trailing bytes, and an intact tail. Experiment 3 survived upload and RELEASE saved interval 53. These are historical observations, not live board readings. Early records before sequence 188 carried `exp=0` from a prototype bug. Claimed-wake work timings are recorded, but the full unattended wake budget and RTC drift remain uncharacterized. Stop an armed board with `LOGGER AUTONOMOUS OFF` during a rendezvous or the 15-second cold-boot maintenance window. Sync, storage ACKs, reclamation, and a storage-full policy remain unimplemented.

**Durable-log recovery is corrected, host-tested and normal-path hardware-smoke validated (2026-09-24):** the working-tree image reported `eba3b5d-dirty`. The full scan distinguishes torn tail, invalid tail, mid-file damage and read error; only tail-only damage is automatically repaired, while detected mid-file damage and read errors preserve the file and refuse repair. `logIntact` includes `!readError`; D-023 sequence authority is preserved. Damaged-log branches remain host/synthetic tested only. No live corruption was injected. Non-record-sized insertion resynchronization and an operator repair/quarantine command remain open. See D-057, STORAGE_SYNC_DESIGN Section 11 and [LAB_NOTES.md](LAB_NOTES.md#2026-09-24-hardware-validation-addendum--clean-record-format-and-working-tree-recovery).

**DIAG_ALRT ordering fixed in source 2026-09-23 and hardware-smoke validated 2026-09-24:** an autonomous wake used to read the INA228 accumulators before `DIAG_ALRT`, and per datasheet SLYS021A Table 7-16 each accumulator read clears its own overflow flag, so `INA_ACCUM_OF` was unreachable and every record reported "no accumulator overflow" regardless. `DIAG_ALRT` is now read first and the order is pinned by a test. An overflow has never been induced on hardware, so the flag has still never been observed to set; records written before the fix carry a `0` that means "not measured", and their charge and energy values are unaffected. The tethered interval close still reads no overflow flag at all. See [BACKLOG.md](BACKLOG.md), [DECISIONS.md](DECISIONS.md) D-020, and [STORAGE_SYNC_DESIGN.md](STORAGE_SYNC_DESIGN.md) Section 4.

The 72-byte durable record layout and its CRC are now **verified against a real Experiment 3 record** (`seq=4445`, `crc=0xFB25DA73`), rebuilt byte-for-byte on the host and checked against the CRC the board stored. `esp_rom_crc32_le(0, ...)` is confirmed to be standard IEEE 802.3 CRC-32, i.e. `zlib.crc32`. The fixture lives in `tests/test_characterization_record.py`: CRC covers bytes 0–67 and is stored at offset 68. Record version remains 1. `record_format` was extracted on 2026-09-24 as a structural move that changed no byte of the layout (D-056); it is host-tested against those golden bytes and **hardware-validated on clean `eba3b5d`**, with old records readable and new records through 5942; the final dump read 4,531 records, invalid 0. DIAG_ALRT ordering also has regression tests and normal-operation hardware smoke; actual induced-overflow flag behavior remains untested.

The INA228 was verified powered throughout the continuous run by probing the 3V3 rail directly across multiple cycles. **Keeping the INA228 converting through the ESP32's sleep costs roughly 0.74 mA and buys continuous hardware CHARGE and ENERGY accumulation; shutting both down reaches 0.33 mA with measurement and accumulation entirely suspended.** Neither is simply better.

One earlier inference has been retracted: the `28.4 mA - 18.6 mA = 9.8 mA` estimate of INA228 consumption is marked NOT VALID in [LAB_NOTES.md](LAB_NOTES.md), because removing the INA228 also halts the firmware into its FATAL loop, so the two readings came from different firmware states. The datasheet figure is 640 µA typical.

The live plot uses host local time, a rolling recent-history window, a fourth chart for cumulative solar charge, interactive hover inspection, and visible Home/−/+/Follow controls. Plot lines and hover cursors persist across updates for responsive interaction. Auto-follow is the default; use `+` and `−` to change the visible time window, then toggle `Follow` or press `f` to control whether that zoomed window tracks the latest data. Plotting changes and remaining visualization work are recorded in [LAB_NOTES.md](LAB_NOTES.md).
