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

## D-017: Experiment lifecycle is separate from storage lifecycle

An experiment is a logical measurement grouping identified by `experiment_id`. Storage is durable telemetry history ordered by a global sequence number. They are different things with different lifetimes.

`RESET YES` may increment `experiment_id` as it does today. It must never erase stored telemetry, reset the global record sequence, or reset synchronization acknowledgement state. Storage reclamation is a separate operation with its own trigger, and freeing space must never be reachable only by destroying experiment state.

The global sequence number must be monotonic across reboot and power loss. Where the two cannot both be guaranteed, **a gap is acceptable and a duplicate is not**: a gap is visible in the data and harmless, while a reused sequence number silently corrupts a host's record of what happened.

## D-018: Autonomous cadence comes from one authoritative setting

The wake, accounting, and storage cadence of the autonomous logger is governed by a single named setting, `autonomous_interval_seconds`, persisted in NVS. Sixty seconds is the development default because it makes bench testing easy. It is not an architectural commitment.

No second hard-coded cadence may exist in the autonomous path. The existing `MEASUREMENT_INTERVAL_MS` belongs to the host-tethered logger and must not be reused as the autonomous cadence.

Every durable record stores the **actual measured** interval duration rather than the configured one, so a log spanning a cadence change, a late wake, or a reboot mid-interval remains correct and self-describing.

ESP32 wake cadence and INA228 conversion cadence are separate concepts. The INA228 continues converting and accumulating CHARGE and ENERGY in hardware while the ESP32 sleeps, so a longer wake interval costs temporal resolution of the voltage, current, power, and temperature snapshots without costing accuracy of integrated charge and energy.

## D-019: Wall-clock time is synchronizable state, never fabricated

The logger must always be able to create durable records using its global sequence number, whether or not absolute time is known. `seq` is the primary ordering key, it never depends on wall-clock time, and `experiment_id` is not a timestamp and must never be used as one.

Records carry an explicit **time-quality field** rather than a single valid bit, because "the clock was correct when this was written" and "the clock was set some time ago and has been drifting since" are different claims that a host must be able to tell apart:

- `UNKNOWN` — no clock has been set this session; `epoch_s` is 0
- `SYNCHRONIZED` — clock set this session, recently enough to trust
- `HOLDOVER` — clock was set this session, but long enough ago that drift is no longer bounded

When absolute time is known it is stored internally as Unix epoch seconds. `epoch_s` and the quality field always travel together, and a host must never read one without the other.

**Setting the clock is one logical operation, `SET_TIME <unix_epoch_seconds>`, with identical semantics on every transport** — USB serial from the Python logger, Wi-Fi via NTP, or a Bluetooth client. Transport may change; record semantics may not. The operation is idempotent and must never rewrite, reorder, or invalidate records already stored.

The build carries the system clock across deep-sleep wake through the RTC domain (`CONFIG_LIBC_TIME_SYSCALL_USE_RTC_HRT=y`). **Absolute time is not assumed to survive complete loss of board power**, and the design must not depend on it doing so.

A host may retrospectively anchor records written before a sync, using the session identifier and session-relative elapsed time each record carries. That is a host-side operation on the host's own store: **the device never rewrites its own records, and never writes a timestamp it did not have.** This is why there is no device-side `DERIVED` quality value — a derived timestamp and a synchronized one must stay distinguishable permanently. Anchoring works for the current power-on session only; a power cut ends the session and destroys the anchor.

Clock drift and periodic resynchronization are normal operating considerations, not failure modes. The RTC slow clock is the internal RC oscillator (`CONFIG_RTC_CLK_SRC_INT_RC=y`), calibrated at boot but not crystal-backed, and nothing in this project may describe it as precision wall-clock hardware. No absolute-time accuracy figure may be published in documentation, firmware output, or host tooling until drift on this board has been measured.

No external RTC is required or added. One becomes worth considering only if a measured drift figure, or a real need for wall-clock continuity across power loss, justifies it.

## D-020: Accumulators are read before they are reset

The INA228 hardware CHARGE and ENERGY registers hold the integral of the entire period since they were last cleared, including any period during which the ESP32 was in deep sleep. Any code path that resets them without first reading them destroys that integral, and destroys it silently.

The current `setup()` does exactly this: it calls `resetInaAccumulators()` and never reads ENERGY or CHARGE. That is harmless only because both deep-sleep power tests suspend interval accounting. The autonomous wake path must not inherit it.

An autonomous wake reads accumulators, reads the `DIAG_ALRT` overflow flags that qualify them, builds and durably stores the record, and only then resets the accumulators for the next interval. Reads come before writes.

A wake must not reconfigure an INA228 that is already configured and already converting. Writing the `ADC_CONFIG` MODE bits interrupts and restarts a conversion in progress, discarding accumulation that had not yet been committed. Reconfiguration belongs on cold boot, where the device state is genuinely unknown.

## D-021: An armed autonomous logger must always offer a guaranteed management window

Measured on hardware 2026-09-11. An armed board could not be stopped over USB, because the Serial command parser runs only from `loop()` and an armed cold boot returned from `setup()` straight into deep sleep. The parser was never reached, so a correctly delivered `LOGGER TEST STOP` was never read. The only remaining recovery was reflashing.

A device that can arm itself into autonomous operation must always provide a bounded, guaranteed period in which management commands are parsed. The window may not depend on catching a short wake, on host timing, or on USB-presence detection.

On this firmware the window opens on a true cold boot, lasts `AUTONOMOUS_COLD_BOOT_MAINTENANCE_MS` (15 seconds during development), announces itself and counts down, and must be entered before any path that sleeps. Timer wakes do not open one, because awake time dominates the power budget and the recovery path already exists at every reset.

A stop that is requested but cannot be persisted must not be followed by deep sleep. Sleeping after an explicit stop request is the same failure this window exists to prevent, so the firmware refuses to sleep and states that the flag is still armed.

## D-022: A command is acknowledged only when the firmware says so

Writing bytes to a serial device proves that the host accepted them. It proves nothing about whether the firmware read, parsed, or executed them.

`tools/send.sh` previously printed `Sent` and `ALL OK` after a successful write. During the 2026-09-11 run it reported exactly that for a `LOGGER TEST STOP` the firmware never processed, and the operator had no way to tell the difference from a real success. A status line that reports intent rather than outcome converts a caught failure back into an uncaught one.

The direct-serial path now waits for the firmware's own `[COMMAND] Received: <COMMAND>` echo and reports `ALL OK` only when that echo is observed. Its exit status reflects the same fact, so a retry loop keeps retrying until the command actually lands. When no echo arrives it says the bytes were written, says the acknowledgement was not observed, and prints the firmware output it did see.

The logger-owned path still queues the command to a file and cannot observe the outcome, so it now says it queued the command and states plainly that it did not see an acknowledgement. It deliberately captures nothing: opening the port there as a second reader is the multi-owner problem the command file exists to avoid, and the logger's own console already shows the response.

Once acknowledged, the direct path keeps reading and printing firmware output, so a direct send behaves like a short one-shot serial monitor instead of proving a command ran and then hiding what it said. Capture ends when the port has been quiet for a configurable period, or at a hard cap, and **the stop reason is always printed**. A capture cut short by the cap says so on stderr and warns that output may be truncated, because output that merely stops looks identical to output that finished.

`ALL OK` continues to refer to the acknowledgement alone. It is printed when the echo is seen, and neither a truncated capture nor an empty one changes it or the exit status, since a reporting limit is not a failed command.

## D-023: The durable log is the sequence authority when it is provably intact

Sequence numbers are reserved from NVS in blocks of 64 so that a crash inside a block abandons the remainder, producing a visible gap instead of a silent reuse. That rule stands: gaps remain acceptable, duplicates remain forbidden.

The implementation applied it too aggressively. Every arm and every cold boot re-reserved unconditionally, and the next sequence was always taken as one past the NVS reservation, so each restart burned 65 numbers whether or not the previous block had been used. One eight-record run plus two cold boots moved the next sequence from 9 to 131.

Nothing but an append ever writes a record, so an intact log tail is also the authority on the highest sequence ever used, and a clean restart may continue directly from it. The NVS reservation is the floor in every other case, because then the tail cannot be trusted to show everything that was written: an empty log may have been cleared after records were synced, a partial or corrupt tail means records were lost, and out-of-order sequences mean a reuse already happened. A reservation is written only when the existing one does not already cover the sequence about to be used.

This holds only while records are never removed. Once pruning exists, a pruned log no longer carries the highest sequence ever used, and the NVS floor has to apply unconditionally again.

## D-024: A record field may only come from a source the writing path has actually initialized

The autonomous timer-wake branch runs at the top of `setup()`, before the ordinary cold-boot initialization, because the INA228 accumulators must be read before anything can reset them. That ordering is correct and stays.

The cost of it is that every global the normal path initializes later is still at its startup value on that path. `experiment_id` was read straight from the `experimentId` global, which `loadCheckpoint()` populates further down `setup()`, so 65 records were stored claiming experiment 0 while the board was running experiment 2. Nothing errored. The field was populated, well-formed, CRC-correct, and wrong.

Any field written on an optimized early path must come from a source that path has actually established: a constant, RTC-retained state guarded by its magic, a value read during that path, or a dedicated read-only load. Reusing a global because it happens to exist is not sufficient, and a field that looks populated is not evidence that it was.

Where a dedicated load is needed, it is a **small read-only helper**, not the full initialization routine. `loadCheckpoint()` cannot be reused here: it is loud enough to distort the wake timings the bench test exists to measure, and it writes four globals belonging to the host-tethered logger. A helper that duplicates part of another function's rules must say so, because the failure mode is the two disagreeing about the same board.

**No silent fallback.** When context cannot be loaded, the record is marked `EXPERIMENT_UNKNOWN`, `experiment_id` is written as `0xFFFFFFFF`, and the firmware prints an explicit error. It never writes a plausible default. The measurement itself stays valid and is still stored: only the attribution is unknown, and conflating the two would discard good data.

## D-025: Sleep policy asks whether any host is connected, never whether USB is

The autonomous state machine decides whether it may sleep by calling `anyHostConnected()`, backed by a transport bitmask of `CONNECTION_USB`, `CONNECTION_WIFI`, and `CONNECTION_BLE`. Only USB is implemented. The other two exist so a Wi-Fi or BLE client can hold the board awake later without the sleep logic learning anything new.

**Electrical presence is not a claim.** A transport becomes active only through an explicit software lease, never because a cable is plugged in. The 2026-09-11 audit settled this: `isPlugged()` is a start-of-frame watchdog whose own core source says it "is known to flap even on a healthy link", and `isConnected()` reads false whenever no terminal holds the port open even with USB attached. Neither can decide whether a board may sleep. Both are still printed as diagnostics, and nothing depends on them.

The lease is what makes this safe in both directions. It is a positive claim from software that is actually talking to the board, and it expires on its own, so a crashed host, a pulled cable, or a laptop suspending all resolve without the host's cooperation. `LOGGER SESSION RELEASE` is an optimization that returns the board immediately; it is never the only recovery path.

## D-026: A host session suspends autonomous sleep without disarming autonomous mode

`LOGGER TEST STOP` and `LOGGER SESSION HOLD` are different operations and conflating them would be expensive. STOP persistently disarms the autonomous test. HOLD keeps the board awake while a host is attached and leaves autonomous mode **armed**.

```text
autonomous armed = YES, host session held = YES
```

is a normal, valid state: a host is driving a board that resumes autonomous operation the moment the host lets go. `LOGGER SESSION RELEASE` ends the session and continues the already-armed autonomous operation.

While a session is held, interval accounting is **not** suspended. Suspension is now keyed to the autonomous state rather than to the armed flag, because a host session is ordinary awake tethered operation and the host connected in order to collect that telemetry. Autonomous records and tethered intervals still never run at the same time: records are written only on timer wakes, and a timer wake cannot happen while the board is awake.

**The transition back is the accounting-sensitive part.** When a session ends, the tethered interval that is open at that moment is closed cleanly before sleeping, recording its real elapsed length. Leaving it open would roll its charge into the first autonomous record, producing an interval made of tethered awake time plus deep-sleep time. Discarding it would be silent charge loss. Closing it is both honest and exactly the clean accumulator baseline the next autonomous interval needs. If the close fails, the firmware **refuses to sleep** and stays awake for diagnosis, because measurement integrity outranks minimizing awake time.

## D-027: Serial output may never block the state machine

`HWCDC` defaults to `tx_timeout_ms = 100` with `max_consec_timeouts = 20`, so a single blocked `Serial` write stalls for about two seconds once the 256-byte TX ring buffer fills and nothing drains it. `isPlugged()` keeps reporting true after a host closes the port, so the driver waits rather than giving up.

On 2026-09-11 that held `setup()` for roughly 74 seconds after a host claimed a rendezvous, which is why `loop()` was never reached and the 15-second host lease never expired. A logging side effect disabled a safety mechanism.

The firmware therefore sets `setTxBufferSize(4096)` before `begin()` and `setTxTimeoutMs(0)` immediately after. Output is dropped rather than waited on when nothing is draining the port.

This is a deliberate trade, not an oversight. Serial output is diagnostics, and diagnostics may never stall measurement, sleep decisions, or lease expiry. Dropping only happens when nothing is reading, which is when the text has no recipient. Unattended capture is the durable record log's job and never Serial's, so nothing that must survive depends on this path.

Any future code that must run on a deadline belongs outside a `Serial.print()` chain, and any long-running initialization must assume it can be preempted by a lease that expires while it runs.

## D-028: The tethered baseline is established when the host claims the board

A host session's measurement interval starts at `LOGGER SESSION HOLD`, not at the end of `setup()`. `HOLD` discards the rendezvous accumulation with an explicit statement, resets the INA228 accumulators once, and starts the interval timestamp. The ordinary initialization further down `setup()` then **skips** its own reset and says so.

The previous ordering reset the accumulators a second time, hundreds of lines and several seconds after the claim, discarding everything measured in between without a word. Combined with D-027 that silently lost about 74 seconds of charge and left `RELEASE` closing a 9 ms interval.

When a baseline cannot be established because the accumulator reset fails, the session is still claimed but the firmware states that the interval is **not** clean, so whatever it eventually closes is not read as a trustworthy measurement of the session.

A short final interval at release is not by itself evidence of loss: when an ordinary 60-second interval closes moments before release, the remainder legitimately is milliseconds. Release therefore reports the session length and how many ordinary intervals closed during it, so the two cases are distinguishable without inspecting the CSV.

## D-029: One host-side implementation of the device and session protocol

Three programs speak the sleeping-device protocol: `app/solar_logger.py`, `tools/send.sh`, and `tools/upload.sh`. They share one implementation in `app/device_session.py`, reached from the shell through `app/device_tool.py`. None of them may reimplement port discovery, rendezvous waiting, acknowledgement handling, or the session lease.

Protocol strings and host timing constants live in that module and nowhere else. Command text, the acknowledgement prefix, baud rate, port glob, ack timeout, keepalive cadence, and response capture windows are defined once. A magic string copied into a caller is how the three drift apart, and drift here means one tool believing it holds a session that another tool's firmware already released.

The module exposes two shapes because callers genuinely differ. `SerialDevice` owns a port for one-shot work and reads it directly. `SessionClient` owns nothing: it writes through a callback and is fed received lines by whoever does own the port. The logger needs the second, because it runs its own read loop and a second reader would steal the telemetry it exists to collect.

**The firmware remains authoritative for its own lease length.** The host mirrors 15 seconds only to reason about margin. If they ever disagree, the firmware wins and the host's keepalive cadence is what changes.

Host abstractions mirror the firmware's transport model rather than assuming USB. A session is a lease held over some transport, and USB is simply the only one implemented on either side today.

## D-030: A command echo proves parsing, not success

The firmware prints `[COMMAND] Received: <COMMAND>` from `processCommand()` **before** dispatching, so it echoes a command it is about to refuse. `LOGGER SESSION HOLD` on a board that is not in autonomous mode is echoed and then rejected.

Treating the echo as success would leave the host convinced it holds a lease that was never granted, reporting a session the board does not have, and skipping the reconnect logic that should have run.

The host therefore tracks two different things. The echo means the command was **parsed**. A separate outcome line means it **succeeded or failed**: `[SESSION] Host session held: ALL OK`, `[SESSION] NOT ALL OK - HOLD refused.`, `: lease renewed for `, `[SESSION] NOT ALL OK - lease NOT renewed.`, `[SESSION] Released: ALL OK`, and `[SESSION] Host lease EXPIRED after`. Session state is keyed on the outcome, never on the echo alone.

A refused hold is not an error. It is how a board that is not in autonomous mode says no lease is needed, and the logger continues in ordinary tethered operation.

## D-031: Autonomous operation is a mode, not a test

Autonomous sleep/wake/store graduated from a bench test to the canonical operating mode on 2026-09-11, after the timer wake, USB rendezvous, host session, keepalive, and release paths were all confirmed on hardware. The commands say so:

```text
LOGGER AUTONOMOUS ON       enable it persistently
LOGGER AUTONOMOUS OFF      disable it persistently
LOGGER AUTONOMOUS STATUS   mode, cadence, storage, session state
```

`LOGGER TEST AUTONOMOUS`, `LOGGER TEST STOP`, and `LOGGER TEST STATUS` still work as aliases so existing scripts and written procedures keep running, and each prints a deprecation notice naming its replacement. A name that is quietly wrong is worse than one that complains, and silently breaking a recorded procedure is worse than both. Host code uses the canonical names only.

**`LOGGER AUTONOMOUS OFF` does not tear down a live host session.** Autonomous mode is the only thing the lease was gating: with it off the board stays awake either way, so dropping the session would break a connected host's keepalives and gain nothing. The session is kept and the new state is reported, so the host learns about it from `STATUS` rather than from its next keepalive suddenly failing. `KEEPALIVE` and `RELEASE` continue to work normally for that session.

Historical entries in [LAB_NOTES.md](LAB_NOTES.md) keep the command names that were actually typed at the time. Rewriting them to the new spelling would falsify the record of what was run.

## D-032: Autonomous cadence is a deadline, not sleep duration

`LOGGER INTERVAL <seconds>` defines the time from one autonomous interval
boundary to the next. The USB rendezvous and all other awake work are inside
that cadence, not added to it. Before deep sleep, firmware computes the target
as `interval_start + configured_interval` and sleeps only for the remaining
time. The remaining duration is measured from retained monotonic session time;
it is never a hardcoded subtraction for the rendezvous window.

If awake work reaches or passes the target, firmware prints an explicit
warning, starts the next interval at the current time, and sleeps one full
configured cadence. The record already being closed retains its actual
measured duration. This avoids both silent drift and an immediate-wake loop.

The accumulator read, durable append, and accumulator reset ordering remains
unchanged. A rendezvous claim still transitions to the ordinary host-session
path rather than changing the autonomous record cadence.

## D-033: `session_elapsed_ms` is monotonic within `boot_id`

The stored field `session_elapsed_ms` means milliseconds since the cold boot
that established the autonomous session. It is monotonic within one
`boot_id`, subject to the normal uint32 rollover limit. A timer wake retains
the session clock in RTC state; a host session released from that wake must
continue that retained clock rather than rebasing it to `millis()`, which
starts near zero after every deep-sleep reboot.

If RTC session state is genuinely lost, firmware rebuilds from durable
storage, starts a new boot/session identity, and explicitly discards the
unknown-duration interval. It does not claim continuity it cannot verify.

## D-034: All autonomous handoffs use the shared deadline scheduler

Unclaimed rendezvous expiry, host `RELEASE`, host lease expiry, cold-boot
resume, explicit arm, and RTC-loss recovery all enter autonomous sleep through
`autonomousDeepSleepAgain()`. The scheduler uses the retained session clock for
timer-wake measurement and timer-wake host handoff paths, and `millis()` for
paths that start a new session on the current boot.

`RELEASE` and lease expiry close the open tethered interval and establish a new
autonomous interval boundary at that handoff. A full configured sleep from
that boundary is therefore intentional; the handoff overhead is before the
new boundary, not silently added to an existing autonomous interval. An
unclaimed rendezvous does not establish a new boundary and sleeps only until
the existing deadline.

## D-035: RELEASE acknowledges before autonomous teardown

`LOGGER SESSION RELEASE` intentionally destroys the USB transport it uses for
its own acknowledgement. The command therefore performs the accounting-safe
handoff preparation and prints its explicit success result, then marks sleep
pending. The main loop waits a bounded 250 ms grace period before entering
autonomous deep sleep.

The grace period is a hard deadline, not a serial timeout or an unbounded
flush. `Serial.setTxTimeoutMs(0)` remains in force, so a missing host cannot
stall lease expiry or the state machine. Lease expiry has no requesting
command to acknowledge and continues directly from safe accounting into the
shared autonomous scheduler.

## D-036: Machine command protocol is separate from diagnostics

The human-readable `[COMMAND] Received: ...` line is not a protocol
acknowledgement. With ESP32 HWCDC `setTxTimeoutMs(0)`, a write can return a
short count when the TX ring is full, and Arduino `Print` callers do not expose
that short write. The first diagnostic line can therefore be absent even when
the command was parsed and later output is visible.

Every parsed command now emits `CMD_ACK,<command>`, followed by
`CMD_RESULT,<command>,<status>` after dispatch. The firmware retries only these
small protocol lines with a 100 ms hard deadline. Ordinary diagnostics retain
the global zero TX timeout and may still be dropped; they cannot stall lease
expiry or measurement scheduling.

The host requires both machine stages for normal success. A transport loss
after a successful result is reported separately and is expected for RELEASE;
bytes-written, parsed, completed, and disconnected are never conflated.

## D-037: Firmware build identity is source-controlled

The firmware prints `[FIRMWARE] Build: solar-logger-protocol-ack-v2` at boot
and through `STATUS`. This deterministic source-controlled identifier marks the
machine-ACK/RESULT protocol generation without depending on wall-clock compile
times or an unavailable Git repository during Arduino CLI compilation.

Superseded in scope by D-038, which keeps this identifier and adds the two
facts it was never meant to carry. The build ID still answers only "which
generation of the command protocol does this image speak".

## D-038: Firmware identity is three separate facts

"What is on this board?" is three questions, and one string cannot answer them.
`Arduino/solar-logger/firmware_version.h` holds all three and the firmware
prints them together at boot, inside `STATUS`, and on the new `VERSION`
command:

```text
[FIRMWARE] Version:  0.1.0-dev
[FIRMWARE] Revision: 063e048-dirty
[FIRMWARE] Build ID: solar-logger-protocol-ack-v2
```

- **Version** is intent, edited by hand and never derived from Git. A commit
  hash states what the source was; it cannot state what the change was for.
- **Revision** is fact: `git rev-parse --short HEAD`, with `-dirty` when the
  sketch directory differs from `HEAD`. Both a `git diff` against tracked files
  and a check for untracked files are needed, because a new `.h` in the sketch
  directory is compiled into the image and is invisible to the first.
- **Build ID** is protocol generation, deliberately coarse, unchanged from
  D-037.

The revision is injected only by `tools/upload.sh`, through
`--build-property compiler.cpp.extra_flags=-DFIRMWARE_GIT_REV="<rev>"`. That
property is empty in the ESP32 platform.txt, so setting it replaces nothing.

**Every other build leaves it undefined and the firmware says so**, printing
`UNKNOWN - not injected by this build`. IDE builds, plain `arduino-cli compile`
and the IntelliSense pass all keep working unchanged; none of them produces an
image that claims a revision it does not have. Both branches were verified in
the linked image on 2026-09-16.

**The compile timestamp is deliberately not part of this.** Two builds of one
source would carry different identities, and two builds of different source
could carry the same one, so it answers neither question correctly.

Version policy while pre-1.0 is in [PROJECT.md](PROJECT.md).

## D-039: The editor's view of the sketch is generated, and verified before it is used

An Arduino `.ino` is not a C++ translation unit. Arduino CLI concatenates the
sketch, prepends `#include <Arduino.h>`, synthesizes forward prototypes, and
compiles the generated `build/sketch/<name>.ino.cpp`. The compilation database
therefore describes a file nobody edits, and cpptools, which matches entries by
path, finds no configuration for the file that is open.

Three further facts were established on 2026-09-16 and each one alone produces
the reported symptom:

- **ESP-IDF include paths reach the compiler only through indirection.** The
  Arduino library paths arrive as plain `-I` flags; every ESP-IDF path arrives
  as `-iprefix <sdk>/include/` plus a GCC response file holding 325
  `-iwithprefixbefore` entries. That split is exactly the reported split:
  `Wire.h`, `Preferences.h`, `WiFi.h` and `LittleFS.h` resolved while
  `soc/soc_caps.h`, `freertos/FreeRTOS.h`, `esp_sleep.h` and `esp_rom_crc.h`
  did not.
- **cpptools excludes `**/.vscode` by default** (`C_Cpp.files.exclude`), and
  the database and the generated sketch mirror were both inside it.
- **cpptools defaults `C_Cpp.errorSquiggles` to `enabledIfIncludesResolve`,**
  which suppresses every other error while any include fails. A second class of
  error was therefore invisible for as long as the first one lasted.

The configuration is generated by `tools/intellisense.sh` and
`tools/intellisense.py`, which resolve all response-file and `-iprefix`
indirection into absolute `-I` flags, so the editor has nothing left to follow.
**No path under `~/Library/Arduino15` is written down anywhere**; every flag
comes from the database Arduino CLI just produced.

**The generator proves its output before it writes it.** It compiles the raw
`.ino` with `-fsyntax-only` using exactly the flags the editor is about to be
handed, and on failure prints the compiler's own diagnostics and leaves the
previous database in place. A configuration that has not been compiled is a
claim, not a fact.

The sketch's side of the bargain is that it declares its own forward
references. Eighteen functions were called above their definitions, which the
real build never noticed because Arduino CLI supplied the prototypes. They are
declared in the sketch now, so the file stands on its own as ordinary C++ and
the verification step catches the next one by name.

## D-040: Waiting for a rendezvous is the default; session-scoped commands never wait

The board's normal state is autonomous sleep, so a missing `/dev/cu.usbmodem*`
is an ordinary condition rather than a failure (D-025). `tools/send.sh` now
waits for the next USB rendezvous by default. `--no-wait` asks for immediate
failure; `--wait` still parses and is now the default.

**`LOGGER SESSION KEEPALIVE` and `LOGGER SESSION RELEASE` are excluded, and
this is the whole point of the rule.** They address a session that already
exists. A session is a lease held over one transport, so when that transport
disappears the session is over: the firmware's 15-second lease expires on its
own and the board resumes autonomous sleep with no host cooperation. Waiting
would deliver a stale command to a board that has since slept, woken, and
opened a *new* rendezvous — possibly one another process just claimed. The
firmware answers `NOT_HELD` when no session is held, so the harmless case is
covered; the harmful case is releasing somebody else's session, and it is not.

These two therefore never wait, and an explicit `--wait` on them is **refused
with exit status 2** rather than silently ignored. Silently ignoring a flag is
the deviation failure, not the safe option.

`LOGGER SESSION HOLD` is not excluded. It *creates* a session rather than
addressing one, so any rendezvous will do and waiting is exactly right.

The classification lives in `app/device_session.py` as
`requires_live_session()`, so all three host tools agree by construction
(D-029). `tools/upload.sh` passes `--no-wait` explicitly, because it runs
inside its own retry loop and owns the decision about a vanished port.

A stronger rule was considered and deliberately not built: recording the port
and time of each successful HOLD, and requiring KEEPALIVE and RELEASE to match
that record. It closes the remaining window — a command sent while a *different*
session is live on a present port — at the cost of host-side session state with
its own staleness problem. It is in [BACKLOG.md](BACKLOG.md) rather than here.

## D-041: `CMD_RESULT` reports completion, never recognition

`processCommand()` emitted `CMD_RESULT,<command>,OK` whenever the dispatcher
matched a branch. Only `SESSION HOLD`, `KEEPALIVE` and `RELEASE` reported a real
outcome. Every other command answered `OK` after refusing, after a failed NVS
write, after a failed format, and after a rejected argument.

`tools/send.sh LOGGER AUTONOMOUS OFF` therefore printed `ALL OK` and exited 0
for a board that was still armed, because `SerialDevice.send_command()` keys
completion on the literal `OK`. That is the 2026-09-11 failure D-022 was written
about, reproduced inside the machine protocol built to prevent it, and it
contradicted D-036's own "bytes-written, parsed, completed, and disconnected are
never conflated".

The contract is now explicit and the three stages stay distinct:

```text
CMD_ACK,<command>             parsed and ACCEPTED FOR EXECUTION
CMD_RESULT,<command>,OK       the command COMPLETED successfully
CMD_RESULT,<command>,ERROR    parsed, but did not complete successfully
```

The wire format is unchanged. `HOLD` keeps `REFUSED` and `RELEASE` keeps
`NOT_HELD`, because those carry information `ERROR` would discard; the host
treats anything other than `OK` as not-completed either way.

Every handler with a meaningful failure mode returns `bool` and that return
reaches the single emission site. Handlers that cannot fail stay `void`.
**A refusal counts as a failure**: a command that was declined did not do what
was asked, and a host retry loop needs to know the difference between "declined"
and "done".

Two cases are deliberately `OK` rather than `ERROR`, because they are idempotent
successes rather than no-ops: arming something already armed, and disarming
something already off. In both the requested end state holds and nothing failed.
`RESET` and `LOGGER STORAGE CLEAR` without their confirmation word are also
`OK` — explaining that confirmation is required is what those commands do.

An unrecognized command is `ERROR`, as before. It never ran, so it cannot have
completed.

The build ID moves to `solar-logger-protocol-ack-v3`, which is exactly what that
identifier exists for: a host can tell a board that reports completion from one
that reports recognition.

**`LOGGER AUTONOMOUS ON` was a second, opposite defect on the same contract.**
It ended in `esp_deep_sleep_start()`, which does not return, so on the path where
it SUCCEEDED no result was ever emitted and the host reported a failure for a
board that had armed correctly. It now uses the same bounded deferred-sleep
handshake D-035 built for `RELEASE`: emit the result, then sleep from `loop()`
after the grace period. The mechanism carries the sleep path with it rather than
assuming RELEASE.

## D-042: A record condition with no spare flag bit gets an impossible sentinel

`readSensor()` returns false without writing any field, and the autonomous wake
cycle built its record from an uninitialized `SensorReading`. A failed snapshot
therefore stored whatever was on the stack as `bus_uV`, `avg_current_uA`,
`avg_power_uW` and `temp_mC`, in a CRC-correct, correctly-sequenced record whose
flags said nothing was wrong. The interval charge and energy were read earlier
and separately and were unaffected.

The record is still stored. Discarding a valid interval integral to avoid
reporting a bad instantaneous sample would lose the more valuable half, and the
same reasoning as D-024 applies: the measurement stays valid, only part of its
context is unknown.

**All eight `flags` bits are assigned** (STORAGE_SYNC_DESIGN.md Section 5) and
the 72-byte layout is frozen, so there is no bit for a ninth condition. D-024's
pattern is flag-plus-sentinel; here only the sentinel is available.

The values are chosen to be physically impossible rather than plausible:

```text
bus_uV         UINT32_MAX = 4294.97 V   against an 85 V part maximum
avg_current_uA INT32_MIN  = -2147 A
avg_power_uW   INT32_MIN  = -2147 kW
temp_mC        INT32_MIN  = -2147483 C
```

A zero fallback was rejected for the reason D-016 gives: zero is a reading.
Reusing `ACCUM_SUSPECT` was rejected because it claims something untrue about
the accumulators, which were read successfully.

`LOGGER STORAGE DUMP` decodes the sentinel and prints `SNAPSHOT_UNREAD`, so the
condition is named rather than left as four strange numbers.

**The flag byte being full is now a constraint on the design, not an
accident.** The next condition that needs marking cannot reuse this trick — two
sentinel conventions would be worse than one schema change — so it forces a
record version bump, and that is a decision to take deliberately rather than
discover. No stored record can contain these values, so no existing record
changes meaning.

## Project practice

These documents are living engineering records. Record substantive changes, measurements, discovered bugs, mistakes and corrections, architecture decisions, and open questions here as part of the same work. Chat history is not the authoritative project record.
