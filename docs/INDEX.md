# BMW Solar Logger Documentation

These documents are the canonical engineering record for the BMW Solar Logger. Update them in the same work as substantive code changes, measurements, corrections, architecture decisions, and open questions. Do not rely on chat history as the authoritative record.

## Documents

- [PROJECT.md](PROJECT.md) - current repository layout, workflow, telemetry, and operating procedures.
- [DECISIONS.md](DECISIONS.md) - architecture decisions and constraints that should remain stable.
- [LAB_NOTES.md](LAB_NOTES.md) - dated bench results, test setup, qualifications, and follow-up work.
- [BACKLOG.md](BACKLOG.md) - agreed design direction that is deliberately not built yet, including the store-and-forward telemetry logger.

## Current Project

The canonical repository is `/Users/afxjzs/dev/projects/solar-charger`.

- Firmware: `Arduino/solar-logger/solar-logger.ino`
- Python logger: `app/solar_logger.py`
- Current telemetry: `data/samples.csv`, `data/intervals.csv`, `data/events.csv`
- Legacy/archive data: `logs/`
- Canonical upload tool: `tools/upload.sh`
- Serial command tool: `tools/send.sh`

The firmware supports persisted bench power tests: `POWER TEST WIFI`, and the deep-sleep test in two variants, `POWER TEST SLEEP` and `POWER TEST SLEEP INA OFF`. Only one may be armed at a time. The deep-sleep test suspends interval accounting and machine-readable CSV output for its duration; the reasoning and limitations are recorded in [PROJECT.md](PROJECT.md) and [DECISIONS.md](DECISIONS.md) D-011 through D-016.

The deep-sleep measurement was taken on 2026-09-10: 1.07 mA in true deep sleep against approximately 28.2-28.6 mA awake, roughly a 26x reduction. The INA228 was verified powered throughout by probing the 3V3 rail directly across multiple cycles, so that figure is the XIAO ESP32-C3 in deep sleep with the INA228 breakout still powered and converting. The INA-OFF variant, which shuts the INA228's ADC down before each sleep, is implemented but not yet measured.

One earlier inference has been retracted: the `28.4 mA - 18.6 mA = 9.8 mA` estimate of INA228 consumption is marked NOT VALID in [LAB_NOTES.md](LAB_NOTES.md), because removing the INA228 also halts the firmware into its FATAL loop, so the two readings came from different firmware states. The datasheet figure is 640 µA typical.

The live plot uses host local time, a rolling recent-history window, a fourth chart for cumulative solar charge, interactive hover inspection, and visible Home/−/+/Follow controls. Plot lines and hover cursors persist across updates for responsive interaction. Auto-follow is the default; use `+` and `−` to change the visible time window, then toggle `Follow` or press `f` to control whether that zoomed window tracks the latest data. Plotting changes and remaining visualization work are recorded in [LAB_NOTES.md](LAB_NOTES.md).
