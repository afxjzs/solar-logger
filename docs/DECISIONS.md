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

## D-010: Interactive plot view is user-controlled

The plot provides visible `Home`, `−`, `+`, and `Follow` buttons in addition to the native Matplotlib toolbar when available. `+` and `−` directly change the shared time window without changing Follow. Follow is an independent toggle: ON shifts the selected-width window to the newest data, while OFF preserves the inspected view. `f` toggles Follow; `h`, `z`, and `p` mirror Home, zoom in, and zoom out. Hover values use `mplcursors` and show local time plus the plotted measurement. Persistent line artists and cursors are updated in place instead of being recreated on every sample, and the redraw path does not block with a pause.

## D-008: Plot interruptions are display metadata

A serial interruption inserts one guarded NaN marker into the in-memory live plot series. Matplotlib breaks its line at NaN, so the old and new segments do not receive a false connecting line. The marker is not written to any telemetry CSV.

This preserves a visible break across upload and Serial reconnect without fabricating telemetry. New experiment boundaries still clear the plot, while host logger restarts reload recent history instead of treating the restart as a new experiment.

## D-009: Cumulative charge uses interval accounting

The live plot's fourth chart uses `running_charge_mAh` from `CSV_DATA` rows and `data/intervals.csv`. It uses the host `captured_at` timestamp for its x-coordinate and updates only when a completed 60-second accounting interval arrives.

The host does not integrate the one-second sample stream again. The chart represents charge delivered through the INA228-measured solar path and is not battery state of charge. The current application does not display or claim to measure voltage-derived battery SOC. Future SOC work should consider BMW IBS integration and/or a rested-voltage model.

## Project practice

These documents are living engineering records. Record substantive changes, measurements, discovered bugs, mistakes and corrections, architecture decisions, and open questions here as part of the same work. Chat history is not the authoritative project record.
