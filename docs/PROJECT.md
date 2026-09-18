# Project

## Purpose

The BMW Solar Logger measures an INA228-powered solar measurement path with an XIAO ESP32-C3 and records live firmware output on a Mac.

The system is currently **host-tethered**: detailed telemetry exists only while the Python logger is attached. The next milestone is autonomous local logging with later synchronization, designed in [STORAGE_SYNC_DESIGN.md](STORAGE_SYNC_DESIGN.md) and not yet implemented.

## Repository

All paths in these documents are relative to the repository root.

| Area | Path |
| --- | --- |
| Firmware | `Arduino/solar-logger/solar-logger.ino` |
| INA228 driver module | `Arduino/solar-logger/ina228.h`, `Arduino/solar-logger/ina228.cpp` |
| Python logger | `app/solar_logger.py` |
| Current sample telemetry | `data/samples.csv` |
| Current interval telemetry | `data/intervals.csv` |
| Current firmware events | `data/events.csv` |
| Legacy/archive data | `logs/` |
| Firmware version and revision | `Arduino/solar-logger/firmware_version.h` |
| Firmware upload | `tools/upload.sh` |
| Firmware command sender | `tools/send.sh` |
| Local validation gate | `tools/check.sh` |
| CSV archiver | `tools/archive-data.py` |
| Shared host/device protocol | `app/device_session.py` |
| Host protocol CLI | `app/device_tool.py` |
| Editor IntelliSense config | `tools/intellisense.sh`, `tools/intellisense.py` |

`data/` is the current telemetry directory. `logs/` is archive/legacy data and is not the live polling destination.

### Firmware modules

The firmware is being split out of `solar-logger.ino` into real `.h`/`.cpp` modules, one stage at a time. The plan and its order are in [BACKLOG.md](BACKLOG.md), and the boundary rules are [DECISIONS.md](DECISIONS.md) D-047.

One module exists. **The INA228 driver**, `ina228.h` and `ina228.cpp`, holds register access, identity, configuration, shutdown and continuous mode, `readSensor()`, and the CHARGE/ENERGY read and reset primitives. When any of them runs is still decided in `solar-logger.ino`, and so is starting `Wire`, which the header states as a precondition. It was extracted on 2026-09-18 with no intended behavior change, and it is compiled and host-tested but **not yet validated on hardware**; see [LAB_NOTES.md](LAB_NOTES.md).

## Development Workflow

Run the Python logger when live telemetry, plotting, and durable CSV capture are needed:

```text
uv run python app/solar_logger.py
```

The logger uses pyserial at 115200 baud, discovers `/dev/cu.usbmodem*`, prints all firmware output, writes host-timestamped telemetry, and reconnects after serial interruption.

Use `tools/upload.sh` as the canonical compile/upload mechanism. Compilation runs before serial coordination because it does not need the USB port.

### Validation

The canonical local check. Run it before every commit, and before and after every modularization stage:

```text
tools/check.sh
```

| Step | Command |
| --- | --- |
| pytest | `uv run pytest`; needs a host C++ compiler as `c++` on `PATH` |
| pyright | `uvx pyright --pythonpath .venv/bin/python` on every file in `app/`, `tools/`, `tests/`, and `main.py` |
| py_compile | `uv run python -m py_compile` on the same files |
| firmware compile | `arduino-cli compile --clean --warnings all --fqbn esp32:esp32:XIAO_ESP32C3 Arduino/solar-logger`; any warning fails the step |
| shell syntax | `bash -n` on every `tools/*.sh` |
| whitespace | `git diff HEAD --check`, tracked files only; untracked files are listed as not covered |

Every step runs even after one fails. The summary comes from each step's exit status and the script exits 1 if anything failed. It uploads nothing and opens no serial port. The compiled image is not the one `tools/upload.sh` flashes, because nothing injects a revision into it. See [DECISIONS.md](DECISIONS.md) D-043.

`--clean` makes every compile a full rebuild; one measured on 2026-09-18 took 23 seconds. It is required: a cached object prints no warnings, so without it a warning fails the gate once and then passes on every later run.

### Tests

```text
uv run pytest
```

Host-side tests. They touch no hardware and open no serial port.

| Module | Covers | Kind |
| --- | --- | --- |
| `tests/test_command_protocol.py` | `CMD_ACK` / `CMD_RESULT` parsing, RELEASE's expected disconnect, D-040 waiting rules | real host code |
| `tests/test_session_semantics.py` | `SessionClient` lease state, the machine RESULT deciding over human lines, results without an ACK and stale results (D-045), the logger's RELEASE report, `resolve_wait()` | real host code |
| `tests/test_autonomous_accounting.py` | the running-total rule, and the wake-cycle source shape for defects 1, 2 and 5 | specification + source |
| `tests/test_autonomous_schedule.py` | the deadline scheduler: its three pure timing functions for every sleep path, handoffs, long sessions, overruns and rollover; how the scheduler and the handoff call them; one strict `xfail` | firmware code run on the host + source |
| `tests/test_characterization_protocol.py` | `tools/send.sh` exit status for every wire outcome; the firmware dispatcher's ACK/RESULT contract, command set and identity markers | real host code + source |
| `tests/test_characterization_policy.py` | refusals before state changes, including HOLD while a sleep is pending; RELEASE and lease expiry resuming autonomous mode without disarming; the deferred-sleep lifecycle | source |
| `tests/test_characterization_record.py` | the 72-byte record layout, CRC coverage and validation | source |
| `tests/firmware_source.py` | reads every sketch file as C++ tokens for the source tests | helper |

The three `test_characterization_*` modules, the source half of the accounting module, and `test_autonomous_schedule.py` are the pre-modularization characterization gate (D-043). They freeze what the monolith does now, so an extraction stage that changes it fails on the host first.

A source test proves the code still has the shape a rule needs. It does not execute firmware. The one exception is `test_autonomous_schedule.py`: it takes three pure timing functions out of the sketch, compiles them with the host's `c++`, and runs them (D-046). A missing compiler fails the run rather than skipping it. The rest of the firmware can only be compiled for the ESP32 until the modularization in [BACKLOG.md](BACKLOG.md) makes more of it host-compilable, so what remains hardware-only is the acceptance sequence in [LAB_NOTES.md](LAB_NOTES.md).

`tests/test_autonomous_accounting.py` is two different things and says so in its own docstring: an executable specification of the running-total rule, and assertions against the real firmware source.

`test_release_can_report_a_failed_handoff` was a strict `xfail` recording a known RELEASE defect. The defect was fixed on 2026-09-18 (D-044), strict mode turned the fix into an XPASS failure as designed, and the marker was removed in the same change.

The suite has one expected failure: `test_after_an_overrun_the_next_record_covers_what_its_charge_covers`, a strict `xfail` recording that an overrun moves the interval boundary without resetting the accumulators. It is recorded in [BACKLOG.md](BACKLOG.md) and is not fixed. When a fix makes it pass, strict mode fails the suite until the marker comes off (D-043).

### Firmware version policy

`Arduino/solar-logger/firmware_version.h` is the one place the human version lives. The firmware prints three different facts, and they answer three different questions — see [DECISIONS.md](DECISIONS.md) D-038 for why they are not one string:

```text
[FIRMWARE] Version:  0.3.0-dev
[FIRMWARE] Revision: 063e048-dirty
[FIRMWARE] Build ID: solar-logger-protocol-ack-v3
```

They appear at boot, on every autonomous wake, inside `STATUS`, and on the `VERSION` command, which exists so the question can be answered inside a short rendezvous window without printing a whole status block.

**Version** is edited by hand. While pre-1.0:

- **Bump the patch** for a bug fix, a diagnostic change, or a correction that leaves the observable behavior of every command and every stored record the same.
- **Bump the minor** when behavior changes: a new or removed command, a change to what a command does or reports, a change to the record format, a change to the autonomous cadence rules, or a change to the host protocol. Reset the patch to zero.
- **`-dev` stays on the version** until an image has been uploaded and confirmed on hardware. Drop it in the commit that records the successful hardware check.
- **1.0.0 is reserved** for firmware that has run unattended in the car and been read back successfully. That has not happened, so the number stays below it.

The version starts at 0.1.0 because this is the first firmware to carry one. It is not a statement about how much has been built.

**Revision** is injected only by `tools/upload.sh`, from `git rev-parse --short HEAD`, with `-dirty` when the sketch directory differs from `HEAD` — including a new untracked file in it. Any other build prints:

```text
[FIRMWARE] Revision: UNKNOWN - not injected by this build (use tools/upload.sh)
```

IDE builds and plain `arduino-cli compile` are unaffected and produce a working image; they simply cannot claim a revision, and say so rather than printing a blank.

**Build ID** marks the command-protocol generation and is deliberately coarse. Bump it when `CMD_ACK`/`CMD_RESULT` semantics, the bounded protocol writer, or the deferred-RELEASE handshake change, and not otherwise.

### Editor IntelliSense

`tools/intellisense.sh` generates the VS Code C/C++ configuration; `.vscode/c_cpp_properties.json` points at `build/intellisense/compile_commands.json`, which the script writes.

```text
tools/intellisense.sh            regenerate the database
tools/intellisense.sh --check    report staleness only, change nothing
```

Run it after adding an `#include`, and after adding a function that is called above where it is defined. `--check` answers "is it stale?" in well under a second; both are also VS Code tasks.

**Nothing in the repository writes down a path under `~/Library/Arduino15`.** Every include path, define, and flag comes from the database Arduino CLI produces. The script resolves the `@response-file` and `-iprefix` indirection the ESP32 core uses, because the editor does not, and that indirection is where the entire ESP-IDF include path lives. See [DECISIONS.md](DECISIONS.md) D-039.

**The script compiles the sketch with the flags it is about to hand the editor, and refuses to write the database if that fails**, leaving the previous configuration in place and printing the compiler's own diagnostics. A configuration nobody compiled is a claim rather than a fact.

The sketch's half of this is a forward-declaration block near the top of `solar-logger.ino`. Arduino CLI synthesizes prototypes into the generated `.ino.cpp` it actually compiles, so a sketch can call a function defined further down and still build — while the file a person edits is not valid C++. Those eighteen declarations are written out now, and the verification step names the next missing one instead of leaving a squiggle to explain.

**The editor entry covers `solar-logger.ino` only.** `ina228.cpp` has no entry of its own: Arduino CLI's entry names the build copy under `build/intellisense/sketch/`. So expect the editor, but not the build, to misreport that file until the script learns to add module entries. Found 2026-09-18 and recorded in [BACKLOG.md](BACKLOG.md). The editor's behavior on it has not been observed.

### Upload ownership

The upload marker is the repository-root file `.upload-in-progress`. Its content protocol is:

```text
REQUESTED -> RELEASED
```

When the Python logger is running, `tools/upload.sh` detects it, writes `REQUESTED`, waits for the logger to close Serial, and waits for the logger to write `RELEASED`. The uploader discovers the current modem port only after release. The logger stays disconnected while the marker remains present, then resumes discovery after the marker is removed.

### Uploading to a sleeping board

Since the board can put itself into deep sleep and expose USB only during a rendezvous window, **"no `/dev/cu.usbmodem*` exists" is an ordinary state rather than a failure.**

`tools/upload.sh` compiles exactly once, then waits for the board to appear and uploads the binary it already built:

```text
[BUILD] Compile successful: ALL OK
[UPLOAD] No XIAO USB port is currently visible.
[UPLOAD] The board may be in autonomous deep sleep.
[UPLOAD] Waiting for the next USB rendezvous...
[UPLOAD] Press Control-C to cancel.
[UPLOAD] XIAO appeared: /dev/cu.usbmodem1101
[UPLOAD] Starting upload attempt 1 using existing build...
```

Start it while the board is asleep and leave it: the next timer wake is caught automatically.

If a rendezvous closes part way through an upload, the script waits for the next one and **retries the same artifact**. It never recompiles on a retry. Compile writes to a pinned build directory with `--output-dir` and every upload reads it back with `--input-dir`, so that is a property of the commands rather than an assumption about the build cache.

Real failures are not retried. After any failed attempt the complete upload output is printed, then the failure is classified: a vanished port or a known device-presence error waits for the next rendezvous, while anything else (wrong FQBN, missing binary, bad arguments, permissions) exits nonzero. The strongest signal is simply whether the port still exists.

The port is rediscovered before every attempt, because the board can come back as a different `usbmodem` number.

There is no short default timeout, since waiting for a sleeping board can legitimately exceed one autonomous cadence. Control-C prints `[UPLOAD] Cancelled by user.`, releases the logger handshake, and exits 130. Tunable with `UPLOAD_PORT_POLL_SECONDS`, `UPLOAD_RETRY_SETTLE_SECONDS`, `UPLOAD_WAIT_HEARTBEAT_SECONDS`, and `UPLOAD_MAX_ATTEMPTS` (0 = unlimited).

`UPLOAD_PORT_POLL_SECONDS` and `UPLOAD_WAIT_HEARTBEAT_SECONDS` work again as of 2026-09-17. They had regressed to doing nothing when port waiting moved into `app/device_tool.py wait-port`: the shell still read them into variables, and nothing passed them on. `wait-port` now takes `--poll-seconds` and `--heartbeat-seconds`, and `tools/upload.sh` passes both.

This workflow has been bench-tested successfully in both cases:

1. Python logger running: the logger released Serial, wrote `RELEASED`, upload succeeded, and the logger reconnected.
2. Python logger not running: the uploader detected no logger, skipped the handshake, and uploaded directly.

If a running logger does not acknowledge `RELEASED` within five seconds, the uploader aborts. It must not fall through to esptool because that could create a two-process Serial race.

The upload script removes `.upload-in-progress` through its exit, interrupt, and termination cleanup traps.

## Host Session Protocol

`app/device_session.py` is the one host-side implementation of the sleeping-device protocol. `app/solar_logger.py`, `tools/send.sh`, and `tools/upload.sh` all use it, and none of them reimplements port discovery, rendezvous waiting, acknowledgement handling, or the session lease. Protocol strings and host timing constants are defined there and nowhere else.

`app/device_tool.py` is the command-line front end the shell scripts call:

```text
device_tool.py find-port                print the port, exit 1 if the board is away
device_tool.py wait-port                block until the next rendezvous
device_tool.py send COMMAND ...         send, require the echo, print the response
device_tool.py session hold|keepalive|release|status
```

**Who owns Serial, in every state:**

| State | Owner |
| --- | --- |
| Logger running | `app/solar_logger.py`. `send.sh` queues through `.serial-command` and opens nothing. |
| Logger not running, one-shot command | `device_tool.py` for the duration of that command only. |
| Upload requested | `esptool`, after the logger acknowledges `RELEASED`. |
| Board asleep | Nobody. There is no device to own. |

Two different things are tracked, because the firmware echoes a command **before** dispatching it and therefore echoes commands it is about to refuse. The echo means parsed; a separate outcome line means succeeded or failed. See [DECISIONS.md](DECISIONS.md) D-030.

### What `CMD_RESULT` means

```text
CMD_ACK,<command>             parsed and ACCEPTED FOR EXECUTION
CMD_RESULT,<command>,OK       the command COMPLETED successfully
CMD_RESULT,<command>,ERROR    parsed, but did not complete successfully
```

**`OK` means the command did what was asked.** Until 2026-09-17 it meant only that the dispatcher recognized the command, so `LOGGER AUTONOMOUS OFF` answered `OK` after an NVS write failure and `tools/send.sh` exited 0 for a board that was still armed. Every handler with a real failure mode now reports its own outcome. A refusal counts as a failure: a command that was declined did not do what was asked.

Two cases are `OK` rather than `ERROR`, because the requested end state holds and nothing failed: arming something already armed, and disarming something already off. `RESET` and `LOGGER STORAGE CLEAR` without their confirmation word are also `OK` — explaining that confirmation is required is what those commands do.

`LOGGER SESSION HOLD` still answers `REFUSED`, `LOGGER SESSION KEEPALIVE` answers `FAILED` when no session is held, and `LOGGER SESSION RELEASE` still answers `NOT_HELD`, because those say more than `ERROR` would. The host treats anything other than `OK` as not completed. The complete vocabulary is those three plus `OK` and `ERROR`, and a test pins it.

The session commands' answers in full, as of 2026-09-18 ([DECISIONS.md](DECISIONS.md) D-044):

```text
HOLD      OK         lease taken or renewed
          REFUSED    autonomous mode is off; no lease is needed
          ERROR      a deferred autonomous sleep is already pending (after
                     RELEASE or LOGGER AUTONOMOUS ON); claim the next rendezvous

RELEASE   OK         session ended, and if armed: handoff prepared and the
                     deferred sleep armed. Sleep itself follows the result.
          ERROR      session ended, but the handoff failed; the board stays
                     awake for diagnosis and the console names the stage
          NOT_HELD   no session was held
```

**A RESULT whose ACK was lost** is accepted as the outcome and always reported as not clean ([DECISIONS.md](DECISIONS.md) D-045). `tools/send.sh` exits by the RESULT and prints `Firmware completed command, but CMD_ACK was NOT observed` rather than `ALL OK`. If the command's ACK arrives after a result was already taken, that result was stale, from an earlier send, and it is discarded with a warning.

The build ID is the way to tell the generations apart: `solar-logger-protocol-ack-v3` reports completion, `-v2` reported recognition. See [DECISIONS.md](DECISIONS.md) D-041.

Running `uv run python app/solar_logger.py` against a sleeping board is now normal: it prints that the board is sleeping, waits for the next autonomous rendezvous, connects, claims a host session, and keeps it alive with a keepalive every 5 seconds against the firmware's 15-second lease. Control-C releases the board and waits for the firmware's machine RESULT. It prints `Board released: ALL OK` only for a `CMD_RESULT,...,OK` whose ACK was seen. It names `ERROR` and `NOT_HELD` as failures, and reports a port that vanished before any result as outcome UNKNOWN, never as success. If the connection is already gone, it says so and notes that the firmware lease timeout resumes autonomous sleep anyway. See D-045.

## Archiving Telemetry

`tools/archive-data.py` moves the three current CSV files into a dated directory and starts fresh empty ones:

```text
data/samples.csv     ->  logs/2026-09-11_132901-exp2/samples.csv
data/intervals.csv   ->  logs/2026-09-11_132901-exp2/intervals.csv
data/events.csv      ->  logs/2026-09-11_132901-exp2/events.csv
```

```text
tools/archive-data.py --dry-run    # show exactly what would happen
tools/archive-data.py              # do it
tools/archive-data.py --exp 3      # name the directory explicitly
```

**This never touches the device.** It opens no serial port, sends no command, and changes nothing in NVS. The experiment id, interval number, running totals, and autonomous storage are all untouched, and the next row the firmware sends continues the same experiment into the new files. Archiving is a host-side file operation and nothing more.

The experiment number in the directory name is read from the `experiment_id` column of each file's last row. When the files disagree, which a reset between one file's last row and another's can legitimately cause, the script prints each value and says which it chose rather than picking silently.

Each new file is created with **the header copied from the file it replaced**, not a hardcoded copy. `open_csv_file()` in `app/solar_logger.py` compares an existing header against its expected columns and exits if they differ, so copying keeps the two in step when the columns change.

It writes an `ARCHIVE.txt` manifest beside the CSVs recording the row counts, experiment number, and time span, so an archive directory explains itself later.

**It refuses to run while `app/solar_logger.py` is running**, and this is the reason it exists as a script rather than three `mv` commands. The logger holds all three files open in append mode for its entire run. Moving a file out from under an open handle does not redirect that handle: the logger would keep writing into the archived file through the same inode while the new file stayed empty. Truncating in place is no better, because the handle keeps its offset and the next append lands past it, leaving a hole of NUL bytes. Either way the data goes somewhere nobody is looking and nothing reports an error. Stop the logger first; stopping it does not affect the device or the experiment.

## Serial Commands

Use `tools/send.sh` from the repository root. Arguments are joined into one newline-terminated firmware command.

### Waiting is the default

The board's normal state is autonomous sleep, so a missing `/dev/cu.usbmodem*` is an ordinary condition rather than a failure. A plain send waits for the next USB rendezvous:

```text
tools/send.sh LOGGER STORAGE INFO

    board awake   ->  sent now
    board asleep  ->  waits for the next rendezvous, then sends
```

`--no-wait` asks for immediate failure instead. `--wait` still parses; it is now the default and says so.

**Two commands never wait, and this is the point of the rule:**

```text
LOGGER SESSION KEEPALIVE
LOGGER SESSION RELEASE
```

Both address a session that already exists. A session is a lease held over one transport, so once the port is gone the session is over — the firmware's 15-second lease expires by itself and the board resumes autonomous sleep with no help from the host. Waiting would deliver a stale command to a board that has since slept, woken, and opened a **new** rendezvous, possibly one another process just claimed. Both fail immediately with an explanation, and an explicit `--wait` on either is **refused with exit status 2** rather than silently ignored.

`LOGGER SESSION HOLD` is not in that set. It creates a session rather than addressing one, so any rendezvous will do and waiting is exactly right:

```text
tools/send.sh LOGGER SESSION HOLD
```

replaces the old retry loop. Exit status is `0` completed (`CMD_RESULT,...,OK`; a lost ACK is warned about, D-045), `1` not, `2` request refused, `130` canceled.

The classification lives in `app/device_session.py` as `requires_live_session()`, so all three host tools agree by construction. `tools/upload.sh` passes `--no-wait` explicitly, because it runs inside its own retry loop. See [DECISIONS.md](DECISIONS.md) D-040.

```text
tools/send.sh WIFI STATUS
tools/send.sh WIFI ON
tools/send.sh WIFI OFF
tools/send.sh STATUS
tools/send.sh VERSION
tools/send.sh HELP
tools/send.sh POWER TEST WIFI
tools/send.sh POWER TEST SLEEP
tools/send.sh POWER TEST SLEEP INA OFF
tools/send.sh POWER TEST STATUS
tools/send.sh POWER TEST STOP
```

When `app/solar_logger.py` is running, `send.sh` writes `.serial-command`. The logger reads the complete command, writes it through its existing pyserial connection, flushes the output, and removes the handoff file. This prevents a second process from opening the port.

When the logger is not running, `send.sh` discovers the current `/dev/cu.usbmodem*` port and sends the command directly at 115200 baud. It refuses to send when `.upload-in-progress` exists, so command sending cannot race a firmware upload. It also refuses to overwrite an existing pending command.

In direct mode it then behaves like a short one-shot serial monitor: it waits for the firmware's `[COMMAND] Received:` echo, reports the acknowledgement, and prints the response that follows.

```text
tools/send.sh LOGGER AUTONOMOUS STATUS     # prints the [AUTO] status block
tools/send.sh LOGGER STORAGE INFO    # prints the storage summary
tools/send.sh LOGGER STORAGE DUMP    # prints the decoded records
```

Capture ends when the port has been quiet for 600 ms, or at a hard cap of 2 seconds (10 seconds for `LOGGER STORAGE DUMP`, the only command that streams one line per stored record). The stop reason is always printed, and a capture that hits the cap warns that output may be truncated.

The 600 ms quiet threshold sits below both the firmware's 1-second CSV cadence and its 5-second heartbeat, so a finished response reads as quiet whether or not `loop()` is running. During normal logging a stray CSV line may still land inside the window; it is printed rather than filtered.

Both windows are tunable per invocation with `--capture-seconds` and `--idle-seconds`, which `tools/send.sh` passes through to `app/device_tool.py`.

The `SEND_RESPONSE_SECONDS`, `SEND_RESPONSE_SECONDS_DUMP` and `SEND_RESPONSE_IDLE_MS` environment variables this section used to document **no longer exist**. They belonged to the pure-shell `send.sh`, and nothing read them after the protocol moved into `app/device_session.py`; setting one had no effect and said nothing. Corrected 2026-09-17 after the architecture review found the drift by grep.

Ordinary commands finish in about a second, which matters because recovery commands have to fit inside the 15-second cold-boot maintenance window.

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

## Flash and Partition Layout

The XIAO ESP32-C3 has 4 MiB of flash. The build uses the stock `default` partition scheme (`build.partitions=default` in the core's `boards.txt`), which allocates it as follows:

| Name | Offset | Size | Used today |
| --- | --- | ---: | --- |
| bootloader + partition table | `0x000000` | 36,864 | yes |
| `nvs` | `0x009000` | 20,480 | yes — experiment state and power-test flags |
| `otadata` | `0x00E000` | 8,192 | no |
| `app0` | `0x010000` | 1,310,720 | yes — the firmware |
| `app1` | `0x150000` | 1,310,720 | no — never used, no OTA |
| `spiffs` | `0x290000` | 1,441,792 | no — entirely free |
| `coredump` | `0x3F0000` | 65,536 | no |

The "Maximum is 1310720 bytes" line in every compile is the `app0` partition size, not the flash size and not a generic ESP32 limit.

No filesystem is mounted. The core's LittleFS defaults to the partition labeled `spiffs`, so 1,441,792 bytes are available for durable local storage without changing the partition table — which means adopting storage cannot disturb the `nvs` partition holding experiment state. See [STORAGE_SYNC_DESIGN.md](STORAGE_SYNC_DESIGN.md).

## Autonomous Storage and Sync

Designed, not implemented. Full design in [STORAGE_SYNC_DESIGN.md](STORAGE_SYNC_DESIGN.md).

The target architecture keeps the INA228 converting continuously while the ESP32 sleeps for a configurable interval, wakes to harvest the hardware CHARGE and ENERGY accumulators into a durable 72-byte binary record, and sleeps again. A host later connects over any transport, asks for records after a given sequence number, stores them, and acknowledges; only acknowledged records become reclaimable.

Two findings from the design audit matter to the current firmware:

- **The present boot path would destroy sleep-period accumulation.** `setup()` calls `resetInaAccumulators()` without ever reading ENERGY or CHARGE, so an autonomous wake reusing it would silently discard every sleep interval's charge. The autonomous wake path must read accumulators before resetting them. This is harmless today only because both deep-sleep power tests suspend interval accounting.
- **Awake time dominates the power budget, not cadence.** With the current 3.5-second boot, a 60-second wake cadence averages about 2.66 mA against a 1.07 mA deep-sleep floor. The 1500 ms USB enumeration delay and 2000 ms ADC settle are both unnecessary on a timer wake with the INA228 already converging.

Wake cadence is governed by one setting, `autonomous_interval_seconds`, defaulting to 60 seconds for development. That is a bench default rather than an architectural commitment, and every record stores its own measured interval duration so a cadence change leaves the log self-describing.

Wall-clock time is treated as synchronizable state rather than something the logger always has. Records are always creatable using the global sequence number, and each one carries a time-quality field distinguishing a clock that has never been set from one set recently and one set long enough ago to have drifted. Setting the clock is a single transport-independent `SET_TIME` operation, so the Python logger over USB, NTP over Wi-Fi, and a Bluetooth client all supply the same thing. The RTC slow clock here is the internal RC oscillator, not a crystal, so drift and periodic resync are normal considerations — and no accuracy figure will be published until drift is measured. No external RTC is required. See D-019.

## Autonomous Logger Bench Test

A bench-only prototype of one autonomous sleep/read/store cycle. Full design in [STORAGE_SYNC_DESIGN.md](STORAGE_SYNC_DESIGN.md).

**Storage is confirmed on hardware.** As of 2026-09-11 the log holds 65 valid 72-byte records, 4680 bytes, tail intact, with hardware-accumulated charge and energy surviving deep sleep and interval durations of 60003 to 60006 ms. **Timings are still unmeasured**, because the first run's instrumentation was measuring the wrong span; it is fixed and needs another run.

**Records before sequence 188 carry `exp=0` because of a known prototype bug**, not because they belong to experiment 0. The autonomous wake branch runs before `loadCheckpoint()`, so the `experimentId` global was still at its startup value when those records were stamped. Records from sequence 188 onward carry the real experiment id, read from NVS on each wake. The old records are deliberately left untouched; see [LAB_NOTES.md](LAB_NOTES.md) and [DECISIONS.md](DECISIONS.md) D-024.

```text
LOGGER AUTONOMOUS ON        enable autonomous sleep/wake/store mode
LOGGER AUTONOMOUS STATUS    mode, cadence, storage, session state
LOGGER AUTONOMOUS OFF       disable it; stored records are NOT deleted
LOGGER INTERVAL <seconds>   the one authoritative cadence setting, 10-3600
LOGGER STORAGE INFO         filesystem and durable-log summary
LOGGER STORAGE DUMP         decode stored records to Serial
LOGGER STORAGE CLEAR YES    erase stored records, never experiment state
```

Each cycle wakes on the timer, validates the INA228 **by reading only**, reads the accumulated CHARGE and ENERGY for the completed interval, reads `DIAG_ALRT` in the same wake because `CHARGEOF` clears when CHARGE is read, takes a V/I/P/temperature snapshot, appends one 72-byte CRC32-protected binary record to LittleFS, and only then resets the accumulators. If the record cannot be stored the accumulators are deliberately left alone, so the next interval covers both periods rather than losing one.

The timer-wake path branches at the top of `setup()`, before any cold-boot initialization, and skips the 1500 ms USB enumeration delay and the 2000 ms ADC settle. Neither applies to a battery-powered timer wake where the INA228 never stopped converting. Cold-boot behavior is unchanged.

**Accounting is isolated.** The running totals in these records are test-local, held in RTC memory and recovered from the durable log. This path never calls `saveCheckpoint()` and never modifies the experiment id, interval number, or experiment running totals. `LOGGER STORAGE CLEAR YES` erases stored records and nothing else — storage lifecycle stays separate from experiment lifecycle, as required by D-017.

Storage uses LittleFS on the stock `spiffs` partition with no partition-table change. Mount never formats automatically: a first-run unformatted partition and a corrupted one look identical at mount time, so initializing is an explicit operator action.

### Connecting to an armed board

Every autonomous timer wake ends with a 10-second USB rendezvous window, so a host can claim the board without any physical intervention.

```text
LOGGER SESSION HOLD       claim this wake; stay awake (autonomous stays ARMED)
LOGGER SESSION KEEPALIVE  renew the 15-second lease
LOGGER SESSION RELEASE    hand the board back to autonomous sleep
LOGGER SESSION STATUS     state, lease, transports, rendezvous
```

`LOGGER SESSION HOLD` is not `LOGGER AUTONOMOUS OFF`. HOLD keeps the board awake while a host is attached and leaves autonomous mode armed; STOP disarms it persistently. `autonomous armed = YES, host session held = YES` is a valid state.

RELEASE and lease expiry start a new autonomous interval at the moment of the handoff, however long the session lasted. The first sleep after them is one cadence minus the time since the handoff: about 250 ms less after RELEASE, a few milliseconds less after lease expiry. Until 2026-09-18 the scheduler counted the awake time twice, which cut that sleep short by the time the board had been awake; see [DECISIONS.md](DECISIONS.md) D-034 and D-046. That fix has not been verified on hardware.

To catch a wake without touching the board:

```bash
tools/send.sh LOGGER SESSION HOLD
```

That now waits for the next rendezvous by itself. The retry loop this section used to recommend —

```bash
while true; do tools/send.sh LOGGER SESSION HOLD && break; sleep 0.2; done
```

— still works and is no longer needed. The waiting moved into `app/device_session.py`, which polls at the same 200 ms and prints a heartbeat during a long wait.

**The rendezvous is confirmed on hardware.** On 2026-09-11 a timer wake exposed USB, `send.sh` claimed it with `LOGGER SESSION HOLD`, and `LOGGER SESSION RELEASE` returned the board to deep sleep, with no reset, BOOT button, power cycle, or replug. Lease expiry and host-session accounting were both broken in that first run and are fixed; see [LAB_NOTES.md](LAB_NOTES.md) for the two tests that still have to pass.

### Stopping an armed board

An armed board sleeps immediately on cold boot, and the Serial command parser runs only from `loop()`, so for a while there was no way to stop it short of reflashing. On a true cold boot the firmware now stays awake for 15 seconds (`AUTONOMOUS_COLD_BOOT_MAINTENANCE_MS`), announces the window, counts down, and parses commands throughout.

To stop an armed board: reset or power-cycle it, then send `LOGGER AUTONOMOUS OFF` within the window. Timer wakes do not open a window and stay as short as possible, because awake time dominates the power budget.

`tools/send.sh` waits for the firmware's `[COMMAND] Received:` echo on the direct-serial path and reports `ALL OK` only when it sees it, so a command that was written but never parsed is reported as a failure rather than a success. It then prints the firmware's response, so `LOGGER AUTONOMOUS STATUS` and `LOGGER STORAGE INFO` can be read during the window without starting the Python logger.

## Firmware Power Tests

The firmware supports two persisted, autonomous bench power tests. Only one may be armed at a time, and the firmware refuses to arm a second one rather than silently replacing the first. `POWER TEST STOP` stops whichever test is active, and `POWER TEST STATUS` reports both.

```text
POWER TEST WIFI
POWER TEST SLEEP
POWER TEST SLEEP INA OFF
POWER TEST STATUS
POWER TEST STOP
```

Neither test resets, increments, migrates, or rewrites the experiment checkpoint.

### Wi-Fi power test

The test repeats these non-blocking phases:

```text
WIFI OFF for 30 seconds
WIFI ON in station mode, not associated with an access point, for 30 seconds
WIFI OFF for 30 seconds
```

The armed flag is stored in the NVS key `wifi_test_armed`, separate from experiment state. It survives USB disconnect, loss of power, external AA battery power, and reboot. The test changes only Wi-Fi state; it does not alter INA228 measurement behavior, and interval accounting continues normally throughout.

### Deep-sleep power test

`POWER TEST SLEEP` measures XIAO ESP32-C3 supply current while the ESP32 is in true deep sleep and the INA228 breakout remains normally powered from XIAO 3V3. The comparison it exists to support is:

```text
normal awake logger
vs
ESP32 deep sleep with INA228 still powered
```

The test repeats:

```text
AWAKE for 30 seconds, Wi-Fi OFF, normal INA228 behavior
DEEP SLEEP for 30 seconds
```

This uses `esp_sleep_enable_timer_wakeup()` and `esp_deep_sleep_start()`. It is real deep sleep, not `delay()` and not light sleep.

**Deep sleep wakes through reset semantics.** `esp_deep_sleep_start()` does not return. The chip reboots and `setup()` runs again from the top, so every wake is a fresh boot and the state machine is split across the reset boundary: `setup()` decides whether an awake phase is starting and why, and `loop()` decides when the awake phase has lasted long enough to sleep again.

The armed flag is stored in the NVS key `sleep_test_arm`, so the test survives USB disconnect and external power cycling. The per-cycle counter lives in RTC-retained memory instead, guarded by a magic value. See [DECISIONS.md](DECISIONS.md) D-011.

**INA228 stays powered in this first test.** VIN, GND, SDA, and SCL all remain connected. Power-gating the INA228 is a separate future experiment; this test deliberately measures "ESP32 deep sleep + INA228 breakout still powered."

Wi-Fi is off for the entire test. The awake-phase banner reports the radio state read back from `WiFi.getMode()` rather than asserting it, and turns the radio off with an explicit error if it finds it on.

#### Wake diagnostics

Every boot prints the wake cause from `esp_sleep_get_wakeup_cause()`:

```text
[BOOT] Wake reason: POWER ON / RESET
[BOOT] Wake cause code: 0
```

or:

```text
[BOOT] Wake reason: DEEP SLEEP TIMER
[BOOT] Wake cause code: 4
```

The raw enum value is printed alongside the name so an unrecognized cause stays diagnosable.

#### Limitations while the deep-sleep test runs

These are real limitations of running a 60-second accounting interval across repeated 30-second deep sleeps, not parser or wiring faults:

- **Interval accounting is SUSPENDED.** `millis()` restarts at zero on every wake, so the 60-second interval timer never expires during a 30-second awake phase. `setup()` also clears the INA228 accumulators on every boot, so hardware accumulation from the previous awake phase is discarded. There is no honest way to report 60 seconds of elapsed interval time for a board that was asleep for half of it. Accounting is therefore gated off explicitly rather than left to fail by arithmetic accident. Experiment ID, interval number, running charge, and running energy are all preserved unchanged.
- **`CSV_SAMPLE` and `CSV_DATA` output is suspended.** `elapsed_seconds` is derived from the frozen interval number and from `millis()`, so it would sawtooth backwards on every wake while `experiment_id` stayed the same. The human-readable heartbeat keeps printing real INA228 values every five seconds.
- **`CSV_EVENT,EXPERIMENT_RESUME` is emitted on every wake,** because the board really did reboot. During the test that is roughly one event per cycle when a host is attached.
- **Each wake costs about 3.5 seconds of extra awake time** before the 30-second awake phase begins: `delay(1500)` for USB re-enumeration plus `delay(2000)` for the ADC to settle. The awake-phase banner prints the measured boot-to-awake-phase time, so the real duty cycle is observable rather than assumed.
- **`RESET YES` is refused** while accounting is suspended. A new experiment created in that state would be frozen from birth and its first interval would never close.

`POWER TEST STOP` clears the persisted flag, ensures Wi-Fi is off, clears the INA228 accumulators, restarts the interval timer, and resumes normal continuous logging. If the accumulators cannot be cleared, accounting deliberately stays suspended and says so, because folding an unaccounted window of unknown length into the next interval would produce a wrong average current and average power that look exactly like right ones.

#### Reproduction procedure

1. Connect the INA228 normally and power the XIAO through VUSB from the 3xAA holder, with the DMM in series.
2. Keep USB connected only long enough to send `POWER TEST SLEEP`.
3. Confirm with `POWER TEST STATUS` that the sleep test is armed.
4. Disconnect USB physically.
5. Apply external battery power and let the board reboot.
6. Watch the DMM alternate between the awake reading and the deep-sleep reading on a roughly 33.5 s / 30 s cycle.
7. Reconnect USB and send `POWER TEST STOP` during an awake phase. The board cannot receive Serial commands while it is asleep.

#### Measured result

Run on 2026-09-10. Full conditions and qualifications are in [LAB_NOTES.md](LAB_NOTES.md).

| Phase | Displayed current |
| --- | ---: |
| Initial boot | approximately 18.8-19.3 mA |
| Normal awake steady state | approximately 28.2-28.6 mA |
| Deep sleep steady state | 1.07 mA |

That is approximately 96.2% lower steady current in deep sleep, roughly a 26x reduction against the ~28.4 mA awake baseline. The sleep reading repeated across multiple cycles with essentially identical values.

**The INA228 was verified as powered throughout.** The XIAO 3V3 / INA228 VIN rail was probed directly across multiple awake and deep-sleep cycles and held steady at approximately 3.28-3.29 V for several minutes, with the INA228 physically attached the whole time. So 1.07 mA is the measured steady supply current of the XIAO ESP32-C3 in true deep sleep with the INA228 breakout still powered.

One thing this measurement does NOT establish: **the wake transient.** The DMM briefly displayed overload during some wake transitions. No peak-current value was captured, and none should be assumed or back-calculated.

It also does not decompose the 1.07 mA into ESP32 and INA228 shares. The rail measurement proves the part was powered, not how much of the total it accounts for.

### Deep-sleep power test, INA-OFF variant

`POWER TEST SLEEP INA OFF` is the same test with one difference: the INA228 is placed in its own shutdown mode immediately before each ESP32 deep sleep. It exists to measure the lowest power reachable **without** physically power-gating the INA228.

Both variants remain available and only one can be armed at a time. Arming the other variant while one is armed is refused; stop first, then arm the one you want. The variant is persisted in the NVS key `sleep_ina_off` alongside the armed flag, so it survives external power cycling — a variant that reset to the default on power-up would run the wrong experiment while `POWER TEST STATUS` reported the right one.

Neither variant removes INA228 power, touches the XIAO 3V3 rail, or disconnects SDA/SCL.

#### How shutdown is entered and verified

Through the MODE bits of `ADC_CONFIG`, per TI datasheet SLYS021A Table 7-6: register address `1h`, reset `FB68h`, MODE in bits 15-12, then VBUSCT, VSHCT, VTCT, and AVG. The datasheet documents two shutdown encodings, `0h` and `8h`; the firmware writes `0h` and accepts either on readback.

Before each sleep the firmware prints the register, writes only the MODE field, reads it back, and verifies:

```text
[POWER TEST] INA-OFF sleep test
[INA228] Entering shutdown mode...
[INA228] ADC_CONFIG before: 0xFB6B
[INA228] ADC_CONFIG after:  0x0B6B
[INA228] Shutdown mode verified: ALL OK
[POWER TEST] Entering ESP32 DEEP SLEEP for 30 seconds...
```

Verification covers two conditions, not one: the MODE bits must read back as a documented shutdown value, **and** the non-MODE bits must be unchanged. A shutdown write that also moved the conversion times or averaging would mean the next wake restored something other than the known-good configuration.

If either check fails, the firmware prints an explicit error, does **not** enter deep sleep, and stops the test. A sleep current measured with the INA228 in an unknown state would be worthless, so refusing to sleep is the correct outcome.

#### Measurement semantics

While the INA228 is shut down, no ADC conversions run. Voltage, current, power, and temperature registers stop updating, and hardware CHARGE and ENERGY accumulation stops. The datasheet notes that registers remain readable in shutdown, which is exactly why this matters: the values are stale, not new.

The firmware refuses to present them as live. While shutdown is active, `STATUS` and the heartbeat both report that the INA228 is shut down and print no measurements rather than printing old ones.

**This variant deliberately sacrifices solar measurement during the sleep phase.** That is its purpose, not a defect.

#### Wake behavior

Deep sleep wakes by reboot, so `setup()` runs its normal path: `configureIna228()` rewrites CONFIG, ADC_CONFIG, and SHUNT_CAL and verifies all three by readback, and the existing 2-second ADC settle is retained. The datasheet's start-up time from shutdown is 60 µs, four orders of magnitude shorter, so no new delay was needed.

On top of that, the INA-OFF path re-reads `ADC_CONFIG` and confirms continuous conversion from the device before saying so:

```text
[BOOT] Wake reason: DEEP SLEEP TIMER
[POWER TEST] Woke from INA-OFF deep sleep: ALL OK
[INA228] Continuous measurement restored: ALL OK
```

If the device does not report continuous mode despite boot configuration reporting success, the firmware reconfigures, re-verifies, and stops the test if it still cannot confirm.

#### Architectural tradeoff

| | ESP asleep + INA continuous | ESP asleep + INA shutdown |
| --- | --- | --- |
| Hardware charge/energy accumulation | preserved | lost during sleep |
| High-resolution samples during sleep | lost | lost |
| Solar measurement during sleep | preserved | sacrificed |
| Measured supply current | 1.07 mA | 0.33 mA |

**Continuous INA228 measurement through the ESP32's sleep costs roughly 0.74 mA, and buys continuous hardware CHARGE and ENERGY accumulation through the sleep window. Shutting both down reaches roughly 0.33 mA, with measurement and accumulation completely suspended for that period.**

Neither option is simply better. 0.74 mA is the price of not having a hole in the charge record, and whether that price is worth paying depends on the deployment.

#### Measured result

Run on 2026-09-10. Full conditions and qualifications are in [LAB_NOTES.md](LAB_NOTES.md).

| Phase | Displayed current |
| --- | ---: |
| Initial cold-start awake | approximately 28.7-28.8 mA |
| Post-deep-sleep awake | approximately 28.3-28.4 mA |
| Deep sleep, INA228 continuous | 1.07 mA |
| Deep sleep, INA228 shutdown | 0.33 mA |

```text
1.07 mA - 0.33 mA = 0.74 mA saved by INA228 shutdown
```

Against the awake baseline, deep sleep with the INA228 shut down is roughly a 98.8% reduction, about 86x.

The datasheet prediction held. SLYS021A specifies IQ at 640 µA typical and 750 µA maximum against IQSD of 2.8 µA typical, predicting a saving near 640 µA before the test was run; the measured 740 µA sits between the typical and maximum figures.

Two things this run did not settle:

- **The wake transient.** The DMM again briefly displayed overload around some wake transitions. Peak current remains uncharacterized.
- **Why cold-start awake current differs from post-sleep awake current** by roughly 0.4 mA. Observed and repeatable, cause not established, no explanation assumed.

#### Next measurement goal

Nothing is scheduled here. The INA228 shutdown question this variant existed to answer is answered. Remaining power work and future concepts are tracked in [BACKLOG.md](BACKLOG.md).
