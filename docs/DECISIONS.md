# Decisions

## D-001: `data/` is live telemetry

The Python logger writes current telemetry to `data/samples.csv`, `data/intervals.csv`, and `data/events.csv`. `logs/` remains archive/legacy data. New code and documentation must use `data/` for live polling output.

## D-002: Upload owns Serial only after logger release

`tools/upload.sh` compiles before serial coordination. When the Python logger is running, the uploader uses `.upload-in-progress` with the content protocol `REQUESTED -> RELEASED`. The uploader must abort if the logger is detected but does not acknowledge release within five seconds. Uploading anyway would allow esptool and the logger to race for the same device.

When no logger process is running, the uploader skips the handshake because no logger can acknowledge it or own the port. It then discovers the current `/dev/cu.usbmodem*` device immediately before upload. The marker is cleaned on success, failure, Ctrl-C, and termination.

## D-003: Commands use a logger-owned handoff

`tools/send.sh` does not open Serial when `app/solar_logger.py` is running. It creates `.serial-command` without overwriting an existing file. The Python logger sends the newline-terminated command through its existing pyserial connection, flushes it, and removes the file.

If the logger is not running, `send.sh` may open the discovered modem directly. If `.upload-in-progress` exists, command sending is refused. These rules keep command sending, live logging, and firmware upload from creating competing Serial owners.

## D-004: Host timestamps supplement firmware time

The Python logger adds timezone-aware local Mac `captured_at` values to samples, intervals, and events. The timestamp approximates when the complete Serial line reached the host. Firmware elapsed time remains in the row because it is the experiment clock and supports relative analysis across an experiment.

## D-005: Existing CSV schemas fail closed

The logger compares an existing CSV header with the expected current header before appending. A mismatch produces an explicit error and stops startup. The logger does not silently place timestamped rows under an old header and does not automatically migrate, rewrite, or delete old data.

## D-006: Wi-Fi bench test state is separate from experiment state

`POWER TEST WIFI` stores its armed state in the separate NVS key `wifi_test_armed`. The test uses `millis()` to run explicit 30-second OFF, ON, and OFF phases without blocking the logger loop. It starts again after reboot when the flag is present.

The Wi-Fi test must not reset, increment, migrate, or rewrite the experiment checkpoint. `POWER TEST STOP` turns Wi-Fi off and clears only the Wi-Fi test flag.

Wi-Fi ON means station mode with no access-point association. That state is useful for measuring the radio subsystem without adding network credentials or network traffic.

## D-007: Host time is the plot coordinate

The live plot uses the timezone-aware host `captured_at` value rather than firmware-relative elapsed time. Firmware elapsed time can move backward after an ESP32 reboot or upload, which creates overlapping x-values. Host capture time remains monotonic across that reboot from the logger's point of view and gives useful local clock labels.

The plot uses fixed-point y-axis formatters with additive offset and scientific notation disabled. Instrumentation values must be visible directly instead of being split between tick labels and a hidden axis offset.

The logger loads only the latest experiment's recent samples at startup and keeps a 15-minute rolling display window during live operation. CSV remains the complete history; the plot window is display-only.

Startup history is loaded before Serial polling begins. Both `samples.csv` and `intervals.csv` are schema-validated, malformed rows are warned about and skipped, and the logger prints exact loaded counts and the time range. This makes a blank restart distinguishable from genuinely empty or invalid history.

## D-008: Plot interruptions are display metadata

A serial interruption inserts one guarded NaN marker into the in-memory live plot series. Matplotlib breaks its line at NaN, so the old and new segments do not receive a false connecting line. The marker is not written to any telemetry CSV.

This preserves a visible break across upload and Serial reconnect without fabricating telemetry. New experiment boundaries still clear the plot, while host logger restarts reload recent history instead of treating the restart as a new experiment.

## D-009: Cumulative charge uses interval accounting

The live plot's fourth chart uses `running_charge_mAh` from `CSV_DATA` rows and `data/intervals.csv`. It uses the host `captured_at` timestamp for its x-coordinate and updates only when a completed 60-second accounting interval arrives.

The host does not integrate the one-second sample stream again. The chart represents charge delivered through the INA228-measured solar path and is not battery state of charge. The current application does not display or claim to measure voltage-derived battery SOC. Future SOC work should consider BMW IBS integration and/or a rested-voltage model.

## D-010: Interactive plot view is user-controlled

The plot provides visible `Home`, `−`, `+`, and `Follow` buttons in addition to the native Matplotlib toolbar when available. `+` and `−` directly change the shared time window without changing Follow. Follow is an independent toggle: ON shifts the selected-width window to the newest data, while OFF preserves the inspected view. `f` toggles Follow; `h`, `z`, and `p` mirror Home, zoom in, and zoom out. Hover values use `mplcursors` and show local time plus the plotted measurement. Persistent line artists and cursors are updated in place instead of being recreated on every sample, and the redraw path does not block with a pause.

## D-011: Deep-sleep test state splits across NVS and RTC memory

`POWER TEST SLEEP` stores its armed flag in the NVS key `sleep_test_arm`, separate from both experiment state and the Wi-Fi test's `wifi_test_armed` key. That flag must survive complete power loss, because the whole point is arming the test over USB and then unplugging USB before applying AA battery power.

The per-cycle counter is stored in RTC-retained memory instead, not NVS. The test sleeps every 30 seconds, which is 2,880 write cycles per day; NVS is flash with finite erase endurance, and the counter has no meaning outside one continuous battery run. RTC slow memory has exactly the right lifetime: retained across deep sleep, lost on power loss.

RTC memory contents are undefined after a power-on reset, so a magic value guards them. Without the guard, a cold boot would read whatever bits happened to be in RAM and report a fabricated cycle number. When the firmware wakes from its own timer but finds the magic missing, it says so explicitly and restarts the counter at 0 rather than printing an invented value.

ESP32 NVS key names allow 15 usable characters. `wifi_test_armed` is exactly 15; `sleep_test_armed` would be 16 and would be rejected, which is why the key is `sleep_test_arm`.

## D-012: Interval accounting is suspended, not silently skipped, during the deep-sleep test

A 60-second accounting interval cannot stay semantically correct across repeated 30-second deep sleeps. `millis()` restarts at zero on every wake, `setup()` clears the INA228 accumulators on every boot, and the board is not awake during sleep. Emitting intervals anyway would produce wrong elapsed time, average current, and average power.

The firmware therefore gates interval accounting off explicitly through a single predicate, announces the suspension when the awake phase starts, and reports it in both `STATUS` and `POWER TEST STATUS`. Relying on the fact that a 30-second awake phase never reaches a 60-second timer would work by arithmetic accident, and nothing would say the logger had stopped accounting.

Experiment ID, interval number, running charge, and running energy are preserved unchanged throughout. `CSV_SAMPLE` and `CSV_DATA` output is suspended for the same reason: `elapsed_seconds` would sawtooth backwards on every wake under an unchanged `experiment_id`. `RESET YES` is refused while accounting is suspended, because a new experiment would be frozen from birth.

The suspension has two causes, both reported by name: the test is running, or a test ended and the INA228 accumulators could not be cleared. The second exists because "the test stopped" and "accounting is safe to resume" are different facts. On a failed accumulator reset the firmware keeps accounting suspended and preserves the cumulative state rather than folding an unaccounted window of unknown length into the next interval.

## D-013: Only one power test may be armed at a time

The Wi-Fi test and the deep-sleep test measure different things and cannot run together. Arming one while the other is armed is refused with an explicit message naming the remedy, rather than silently replacing the armed test and changing what the DMM is measuring.

The two flags live under two NVS keys, which makes one invalid combination representable: both armed. This firmware never creates that state, but if it is ever found at boot the firmware refuses to start either test, says why, and continues normal logging. Guessing which test the user meant would be worse than doing neither.

`POWER TEST STOP` is the single stop path for both tests. It clears whichever flags are set, ensures Wi-Fi is off, resumes interval accounting when it was suspended, and reports failure explicitly for each step that does not succeed.

## D-014: The deep-sleep test has two variants under one armed flag

`POWER TEST SLEEP` and `POWER TEST SLEEP INA OFF` are two variants of one test, not two independent tests. The armed flag lives in the NVS key `sleep_test_arm` and the variant selector in `sleep_ina_off`, which is only meaningful while the armed flag is true.

One armed flag plus one variant flag makes "both variants armed at once" unrepresentable. Two independent armed flags would have added a third invalid state to detect and report, on top of the Wi-Fi/sleep conflict already handled in D-013.

Both flags must be loaded at boot. A variant that silently defaulted after a power cycle would run the wrong experiment while `POWER TEST STATUS` reported the right one, and the DMM reading would be attributed to the wrong configuration. Arming one variant while the other is armed is refused rather than silently swapped, for the same reason D-013 refuses to swap tests: changing what the meter is measuring without saying so is worse than doing nothing.

## D-015: INA228 shutdown is verified against the device, never assumed

The INA-OFF variant enters shutdown through the MODE bits of `ADC_CONFIG`, per TI datasheet SLYS021A Table 7-6, which documents `0h` and `8h` as Shutdown and `Fh` as continuous bus voltage, shunt voltage and temperature. The firmware writes `0h` and accepts either encoding on readback, because the meaningful question is whether the device reports a documented shutdown mode rather than whether it echoes the chosen bit pattern.

Only the MODE field is written. The firmware verifies two conditions before sleeping: that the MODE bits read back as shutdown, and that the non-MODE bits are unchanged. A write that also disturbed VBUSCT, VSHCT, VTCT, or AVG would mean the next wake restored something other than the known-good configuration, and nothing downstream would notice.

If either check fails, the firmware prints an explicit error and does **not** enter deep sleep. A sleep-current reading taken with the INA228 in an unverified state would look exactly like a valid measurement, so the test stops instead.

Restoration is verified the same way. The normal wake path already reconfigures and reads back through `configureIna228()`; the INA-OFF path additionally re-reads `ADC_CONFIG` and confirms continuous mode from the device before printing that measurement was restored.

## D-016: Stale INA228 registers are never reported as measurements

The INA228 answers register reads while shut down, and returns values left over from before shutdown. Printing those next to an "ALL OK" would present stale data as a live measurement.

While shutdown is active, `STATUS` and the heartbeat both state that the INA228 is shut down and report no values at all, rather than reporting old ones. This is the same principle as D-005 and D-008: the system may lose data, but it must never present something plausible in place of something real.

The INA-OFF variant sacrifices solar measurement during every sleep phase by design. That loss is documented and announced when the test is armed and at the start of every awake phase; it is not inferred by the reader from missing rows.

## Project practice

These documents are living engineering records. Record substantive changes, measurements, discovered bugs, mistakes and corrections, architecture decisions, and open questions here as part of the same work. Chat history is not the authoritative project record.
