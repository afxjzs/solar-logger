#!/usr/bin/env python3

"""
BMW Solar Logger - Mac telemetry capture

The ESP32 sends several kinds of text over USB Serial.

Machine-readable messages:

    CSV_SAMPLE,...
        Instantaneous measurement.
        Currently sent once per second.

    CSV_DATA,...
        Completed 60-second accounting interval.

    CSV_EVENT,...
        Experiment lifecycle information.

Everything else is human-readable diagnostic output from the firmware.

This program:

1. Finds the XIAO serial port.
2. Connects at 115200 baud.
3. Prints ALL firmware output.
4. Saves CSV_SAMPLE rows to data/samples.csv.
5. Saves CSV_DATA rows to data/intervals.csv.
6. Displays live voltage, current, and power.
7. Automatically tries to reconnect if the ESP32 disappears.
"""

import csv
import math
import sys
import time
from datetime import datetime
from pathlib import Path

import matplotlib.pyplot as plt
import matplotlib.dates as mdates
import mplcursors
from matplotlib.ticker import FormatStrFormatter
from matplotlib.widgets import Button
import serial
from serial.tools import list_ports


# ============================================================================
# PATHS
# ============================================================================

# __file__ is the path of THIS Python source file:
#
#     .../solar-charger/app/solar_logger.py
#
# .resolve() converts it to an absolute path.
#
# .parent gives:
#
#     .../solar-charger/app
#
# .parent.parent therefore gives the repository root:
#
#     .../solar-charger
#
PROJECT_ROOT = Path(__file__).resolve().parent.parent

DATA_DIR = PROJECT_ROOT / "data"
UPLOAD_HANDSHAKE_FILE = PROJECT_ROOT / ".upload-in-progress"
SERIAL_COMMAND_FILE = PROJECT_ROOT / ".serial-command"

SAMPLES_FILE = DATA_DIR / "samples.csv"
INTERVALS_FILE = DATA_DIR / "intervals.csv"
EVENTS_FILE = DATA_DIR / "events.csv"


# ============================================================================
# SERIAL CONFIGURATION
# ============================================================================

BAUD_RATE = 115200

# How long readline() waits before returning when no Serial data arrives.
SERIAL_TIMEOUT_SECONDS = 1

# How long we wait between reconnect attempts after the ESP32 disappears.
RECONNECT_DELAY_SECONDS = 1

# An open serial device does not prove that the sketch is running. The ESP32
# may instead be in its ROM downloader, resetting, or otherwise silent.
SILENT_WARNING_SECONDS = 3
SILENT_DISCONNECT_SECONDS = 10

# Matplotlib stores only this recent display window. CSV files remain the
# durable, complete history and are never trimmed by the plotter.
PLOT_HISTORY_SECONDS = 900

LOCAL_TIMEZONE = datetime.now().astimezone().tzinfo
PLOT_STATE = {
    "auto_follow": True,
    "cursors": [],
    "buttons": [],
    "lines": [],
    "status_text": None,
    "axes": [],
    "follow_button": None,
    "window_seconds": PLOT_HISTORY_SECONDS,
}


# ============================================================================
# CSV SCHEMAS
# ============================================================================

# CSV_SAMPLE firmware format. captured_at is added by the Mac logger and is
# intentionally separate from the ESP32's experiment-relative time.
#
# CSV_SAMPLE,
# experiment_id,
# elapsed_seconds,
# voltage_V,
# current_mA,
# power_mW,
# temperature_C
#
SAMPLE_FIRMWARE_COLUMNS = [
    "experiment_id",
    "elapsed_seconds",
    "voltage_V",
    "current_mA",
    "power_mW",
    "temperature_C",
]

SAMPLE_COLUMNS = ["captured_at", *SAMPLE_FIRMWARE_COLUMNS]


# CSV_DATA firmware format:
#
# CSV_DATA,
# experiment_id,
# interval,
# elapsed_seconds,
# voltage_V,
# current_mA,
# power_mW,
# temperature_C,
# interval_charge_mAh,
# interval_energy_mWh,
# average_current_mA,
# average_power_mW,
# running_charge_mAh,
# running_energy_mWh
#
INTERVAL_FIRMWARE_COLUMNS = [
    "experiment_id",
    "interval",
    "elapsed_seconds",
    "voltage_V",
    "current_mA",
    "power_mW",
    "temperature_C",
    "interval_charge_mAh",
    "interval_energy_mWh",
    "average_current_mA",
    "average_power_mW",
    "running_charge_mAh",
    "running_energy_mWh",
]

INTERVAL_COLUMNS = ["captured_at", *INTERVAL_FIRMWARE_COLUMNS]

EVENT_COLUMNS = [
    "captured_at",
    "event_type",
    "experiment_id",
    "detail",
]


# ============================================================================
# SERIAL PORT DISCOVERY
# ============================================================================

def find_xiao_port():
    """
    Find the most likely XIAO ESP32-C3 serial port.

    macOS serial devices suitable for initiating a connection normally use
    /dev/cu.* names.

    The board appears as a /dev/cu.usbmodem* device. The exact suffix can
    change, so discovery must not depend on one fixed port name.
    """

    ports = list(list_ports.comports())

    usbmodem_ports = [
        port
        for port in ports
        if port.device.startswith("/dev/cu.usbmodem")
    ]

    if len(usbmodem_ports) == 1:
        return usbmodem_ports[0].device

    if len(usbmodem_ports) > 1:
        print("[WARNING] Multiple USB modem serial ports found:")

        for port in usbmodem_ports:
            print(f"          {port.device} - {port.description}")

        print(
            "[WARNING] Using the first USB modem port. "
            "We should improve board identification if this becomes ambiguous."
        )

        return usbmodem_ports[0].device

    return None


def read_upload_handshake():
    """
    Read the upload coordination file's content.

    None means that no upload is in progress. An empty string is retained as
    an in-progress state because upload.sh may be between writing the file and
    writing its complete state; reconnecting during that window would be
    unsafe.
    """

    if not UPLOAD_HANDSHAKE_FILE.exists():
        return None

    try:
        return UPLOAD_HANDSHAKE_FILE.read_text(encoding="utf-8").strip()

    except OSError as error:
        print(
            "[SERIAL] ERROR: Could not read upload handshake: "
            f"{error}"
        )
        return ""


def close_serial_connection(connection):
    """Close a serial connection and report close failures explicitly."""

    try:
        connection.close()

    except (serial.SerialException, OSError) as error:
        print(f"[SERIAL] ERROR while closing port: {error}")
        return False

    return True


def remove_pending_command():
    """Remove a rejected command file and report filesystem failures."""

    try:
        SERIAL_COMMAND_FILE.unlink()

    except FileNotFoundError:
        return True

    except OSError as error:
        print(f"[COMMAND] ERROR: Could not remove pending command: {error}")
        return False

    return True


def send_pending_command(ser, sent_command):
    """
    Send one command handed off by tools/send.sh.

    The newline is required because the firmware processes a command only
    when it receives the line terminator. A failed send is rejected explicitly
    and removed so it cannot execute unexpectedly after a later reconnect.
    """

    if not SERIAL_COMMAND_FILE.exists():
        return None

    try:
        command = SERIAL_COMMAND_FILE.read_text(encoding="utf-8")

    except OSError as error:
        print(f"[COMMAND] ERROR: Could not read pending command: {error}")
        return sent_command

    if not command.endswith("\n"):
        print("[COMMAND] ERROR: Pending command is incomplete; rejecting it.")
        remove_pending_command()
        return None

    command = command[:-1]

    if not command or "\n" in command or "\r" in command:
        print("[COMMAND] ERROR: Pending command is malformed; rejecting it.")
        remove_pending_command()
        return None

    if ser is None:
        print(
            "[COMMAND] ERROR: Serial is not connected; command was not sent: "
            f"{command}"
        )
        remove_pending_command()
        return None

    if command == sent_command:
        return sent_command

    try:
        ser.write((command + "\n").encode("utf-8"))
        ser.flush()

    except (serial.SerialException, OSError) as error:
        print(
            "[COMMAND] ERROR: Could not send command over Serial: "
            f"{error}"
        )
        remove_pending_command()
        return None

    print(f"[COMMAND] Sending to ESP32: {command}")
    if not remove_pending_command():
        print(
            "[COMMAND] ERROR: Command was sent but remains pending; "
            "it will not be sent again during this logger run."
        )
        return command

    return None


# ============================================================================
# CSV PARSING
# ============================================================================

def parse_sample(line):
    """
    Convert one CSV_SAMPLE line into a Python dictionary.

    Example:

        CSV_SAMPLE,2,5166.632,13.108789,337.584000,4425.356800,28.1563

    line.split(",") produces:

        [
            "CSV_SAMPLE",
            "2",
            "5166.632",
            "13.108789",
            "337.584000",
            "4425.356800",
            "28.1563",
        ]

    The first item identifies the message type.

    The remaining six fields contain the data.
    """

    parts = line.split(",")

    expected_parts = len(SAMPLE_FIRMWARE_COLUMNS) + 1

    if len(parts) != expected_parts:
        print(
            "[WARNING] CSV_SAMPLE field count is wrong. "
            f"Expected {expected_parts}, got {len(parts)}"
        )
        return None

    values = parts[1:]

    row = dict(zip(SAMPLE_FIRMWARE_COLUMNS, values))

    # Experiment IDs are integers.
    row["experiment_id"] = int(row["experiment_id"])

    # All the measurements are floating-point values.
    for column in SAMPLE_FIRMWARE_COLUMNS[1:]:
        row[column] = float(row[column])

    return row


def parse_interval(line):
    """
    Convert one CSV_DATA interval summary into a Python dictionary.
    """

    parts = line.split(",")

    expected_parts = len(INTERVAL_FIRMWARE_COLUMNS) + 1

    if len(parts) != expected_parts:
        print(
            "[WARNING] CSV_DATA field count is wrong. "
            f"Expected {expected_parts}, got {len(parts)}"
        )
        return None

    values = parts[1:]

    row = dict(zip(INTERVAL_FIRMWARE_COLUMNS, values))

    row["experiment_id"] = int(row["experiment_id"])
    row["interval"] = int(row["interval"])

    for column in INTERVAL_FIRMWARE_COLUMNS[2:]:
        row[column] = float(row[column])

    return row


def parse_event(line):
    """
    Convert one variable-length CSV_EVENT line into a Python dictionary.

    The event tail is preserved as one comma-joined detail field because
    different event types may carry different numbers of detail values.
    """

    parts = line.split(",")

    if len(parts) < 3:
        print(
            "[WARNING] CSV_EVENT is malformed. Expected at least 3 fields, "
            f"got {len(parts)}: {line}"
        )
        return None

    if not parts[1]:
        print(f"[WARNING] CSV_EVENT has an empty event_type: {line}")
        return None

    try:
        experiment_id = int(parts[2])

    except ValueError:
        print(
            "[WARNING] CSV_EVENT has an invalid experiment_id: "
            f"{line}"
        )
        return None

    return {
        "event_type": parts[1],
        "experiment_id": experiment_id,
        "detail": ",".join(parts[3:]),
    }


# ============================================================================
# CSV FILE HANDLING
# ============================================================================

def current_host_timestamp():
    """Return local Mac time with its UTC offset in ISO 8601 format."""

    # captured_at is host time because the Mac receives the complete serial
    # line; the ESP32 does not provide a synchronized wall clock. The
    # timezone-aware value keeps the local UTC offset instead of creating an
    # ambiguous timestamp. elapsed_seconds remains useful for experiment
    # timing because it is measured by the firmware's experiment clock.
    return datetime.now().astimezone().isoformat()


def parse_captured_at(value):
    """Parse one stored ISO 8601 timestamp and require timezone information."""

    if isinstance(value, datetime):
        captured_at = value
    else:
        captured_at = datetime.fromisoformat(value)

    if captured_at.tzinfo is None or captured_at.utcoffset() is None:
        raise ValueError("captured_at is not timezone-aware")

    return captured_at


def open_csv_file(path, columns):
    """
    Open a CSV file for append.

    If the file does not already exist, write the column header first. Existing
    files must have the exact current header before append; otherwise old data
    and new rows would have different meanings in one CSV file.
    """

    file_exists = path.exists()

    if file_exists:
        with path.open("r", newline="") as existing_handle:
            existing_header = next(csv.reader(existing_handle), [])

        if existing_header != columns:
            print(f"[FILE] ERROR: CSV schema mismatch: {path}")
            print(f"[FILE] Existing header: {existing_header}")
            print(f"[FILE] Expected header: {columns}")
            print(
                "[FILE] ERROR: Refusing to append. Archive or rename the "
                "existing file before restarting."
            )
            raise SystemExit(1)

    handle = path.open(
        "a",
        newline="",
        buffering=1,
    )

    writer = csv.DictWriter(
        handle,
        fieldnames=columns,
    )

    if not file_exists:
        writer.writeheader()
        print(f"[FILE] Created {path}")

    else:
        print(f"[FILE] Appending to {path}")

    return handle, writer


def load_recent_samples(path):
    """Load the latest experiment's recent valid samples for the plot."""

    print(f"[PLOT] Loading recent history from {path}...")

    if not path.exists():
        print("[PLOT] No historical samples available. Starting with empty plot.")
        return []

    valid_rows = []

    try:
        with path.open("r", newline="") as handle:
            reader = csv.DictReader(handle)

            if reader.fieldnames != SAMPLE_COLUMNS:
                print(
                    "[PLOT] ERROR: Historical samples header changed while "
                    "loading; refusing to plot it."
                )
                return []

            for row_number, row in enumerate(reader, start=2):
                try:
                    row["experiment_id"] = int(row["experiment_id"])
                    for column in SAMPLE_FIRMWARE_COLUMNS[1:]:
                        row[column] = float(row[column])
                    row["captured_at"] = parse_captured_at(row["captured_at"])

                except (KeyError, TypeError, ValueError) as error:
                    print(
                        "[PLOT] WARNING: Skipping malformed historical sample "
                        f"at row {row_number}: {error}"
                    )
                    continue

                valid_rows.append(row)

    except OSError as error:
        print(f"[PLOT] ERROR: Could not read historical samples: {error}")
        return []

    if not valid_rows:
        print("[PLOT] No historical samples available. Starting with empty plot.")
        return []

    newest_sample = max(valid_rows, key=lambda row: row["captured_at"])
    latest_experiment_id = newest_sample["experiment_id"]
    newest_time = newest_sample["captured_at"]
    cutoff = newest_time.timestamp() - PLOT_HISTORY_SECONDS

    recent_rows = [
        row
        for row in valid_rows
        if (
            row["experiment_id"] == latest_experiment_id
            and row["captured_at"].timestamp() >= cutoff
        )
    ]
    recent_rows.sort(key=lambda row: row["captured_at"])

    print(f"[PLOT] Latest experiment: {latest_experiment_id}")
    print(
        f"[PLOT] Loaded {len(recent_rows)} samples covering "
        f"{PLOT_HISTORY_SECONDS // 60} minutes."
    )
    print("[PLOT] Historical plot initialization: ALL OK")
    return recent_rows


def load_recent_intervals(path, experiment_id=None):
    """Load recent interval accounting rows for the displayed experiment."""

    print(f"[PLOT] Loading recent charge history from {path}...")

    if not path.exists():
        print("[PLOT] No historical charge intervals available.")
        return []

    valid_rows = []

    try:
        with path.open("r", newline="") as handle:
            reader = csv.DictReader(handle)

            if reader.fieldnames != INTERVAL_COLUMNS:
                print(
                    "[PLOT] ERROR: Historical intervals header does not match "
                    "the current schema; refusing to plot it."
                )
                return []

            for row_number, row in enumerate(reader, start=2):
                try:
                    row["experiment_id"] = int(row["experiment_id"])
                    row["running_charge_mAh"] = float(row["running_charge_mAh"])
                    row["captured_at"] = parse_captured_at(row["captured_at"])

                except (KeyError, TypeError, ValueError) as error:
                    print(
                        "[PLOT] WARNING: Skipping malformed historical interval "
                        f"at row {row_number}: {error}"
                    )
                    continue

                valid_rows.append(row)

    except OSError as error:
        print(f"[PLOT] ERROR: Could not read historical intervals: {error}")
        return []

    if experiment_id is None and valid_rows:
        newest_row = max(valid_rows, key=lambda row: row["captured_at"])
        experiment_id = newest_row["experiment_id"]

    valid_rows = [
        row for row in valid_rows
        if row["experiment_id"] == experiment_id
    ]

    newest_time = max((row["captured_at"] for row in valid_rows), default=None)

    if newest_time is None:
        print(
            f"[PLOT] No historical charge intervals for experiment "
            f"{experiment_id}."
        )
        return []

    cutoff = newest_time.timestamp() - PLOT_HISTORY_SECONDS
    recent_rows = [
        row
        for row in valid_rows
        if row["captured_at"].timestamp() >= cutoff
    ]
    recent_rows.sort(key=lambda row: row["captured_at"])
    print(
        f"[PLOT] Loaded {len(recent_rows)} charge intervals covering "
        f"{PLOT_HISTORY_SECONDS // 60} minutes."
    )
    return recent_rows


# ============================================================================
# LIVE GRAPH
# ============================================================================

def create_graphs():
    """
    Create the live telemetry window.

    These graphs use CSV_SAMPLE, so they update every second.
    """

    plt.ion()

    figure, axes = plt.subplots(
        4,
        1,
        figsize=(11, 10),
        sharex=True,
    )

    figure.subplots_adjust(
        top=0.90,
        bottom=0.12,
        hspace=0.30,
    )

    figure.suptitle("BMW Solar Logger")
    figure.canvas.mpl_connect("key_press_event", handle_plot_key)
    figure.canvas.mpl_connect("button_press_event", handle_plot_press)
    figure.canvas.mpl_connect("button_release_event", handle_plot_release)
    add_plot_controls(figure)
    initialize_plot_lines(figure, axes)
    print(f"[PLOT] History window: {PLOT_HISTORY_SECONDS // 60} minutes")
    print("[PLOT] Auto-follow: ON")
    print("[PLOT] Hover inspection: enabled")

    return figure, axes


def set_auto_follow(enabled):
    """Set whether redraws follow new data or preserve the user's view."""

    if PLOT_STATE["auto_follow"] == enabled:
        return

    PLOT_STATE["auto_follow"] = enabled
    print(f"[PLOT] Auto-follow: {'ON' if enabled else 'OFF'}")

    if PLOT_STATE["status_text"] is not None:
        PLOT_STATE["status_text"].set_text(
            f"Auto-follow: {'ON' if enabled else 'OFF'}"
        )
        PLOT_STATE["status_text"].figure.canvas.draw_idle()

    if PLOT_STATE["follow_button"] is not None:
        PLOT_STATE["follow_button"].label.set_text(
            "Follow ON" if enabled else "Follow OFF"
        )


def add_plot_controls(figure):
    """Add visible controls because the native backend toolbar may be hidden."""

    control_specs = [
        ("Home", 0.55, reset_plot_view),
        ("-", 0.64, zoom_out),
        ("+", 0.73, zoom_in),
        ("Follow", 0.82, toggle_follow),
    ]

    for label, left, callback in control_specs:
        button_axis = figure.add_axes([left, 0.035, 0.075, 0.04])
        button = Button(button_axis, label)
        button.on_clicked(callback)
        PLOT_STATE["buttons"].append(button)

    PLOT_STATE["status_text"] = figure.text(
        0.08,
        0.055,
        "Auto-follow: ON",
        ha="left",
        va="center",
    )
    PLOT_STATE["follow_button"] = next(
        button
        for button in PLOT_STATE["buttons"]
        if button.label.get_text() == "Follow"
    )
    PLOT_STATE["follow_button"].label.set_text("Follow ON")


def get_plot_toolbar(event):
    """Return the active Matplotlib toolbar, if this backend provides one."""

    canvas = getattr(event, "canvas", None)
    manager = getattr(canvas, "manager", None)
    return getattr(manager, "toolbar", None)


def reset_plot_view(event=None):
    set_auto_follow(True)
    print("[PLOT] Home: auto-follow restored.")


def toggle_plot_zoom(event=None):
    toolbar = get_plot_toolbar(event) if event is not None else None

    if toolbar is None:
        print("[PLOT] WARNING: Zoom control is unavailable in this backend.")
        return

    set_auto_follow(False)
    toolbar.zoom()


def toggle_plot_pan(event=None):
    toolbar = get_plot_toolbar(event) if event is not None else None

    if toolbar is None:
        print("[PLOT] WARNING: Pan control is unavailable in this backend.")
        return

    set_auto_follow(False)
    toolbar.pan()


def set_plot_window(seconds):
    """Change the shared visible time window without touching stored data."""

    axes = PLOT_STATE["axes"]

    if len(axes) == 0:
        print("[PLOT] WARNING: Plot axes are not ready for zoom.")
        return

    current_start, current_end = axes[0].get_xlim()
    current_center = (current_start + current_end) / 2
    current_width = current_end - current_start
    new_width = seconds / 86400

    if current_width <= 0:
        print("[PLOT] WARNING: Current plot window is invalid.")
        return

    if seconds < current_width * 86400:
        new_center = current_center
    else:
        new_center = current_end

    for axis in axes:
        axis.set_xlim(
            new_center - new_width / 2,
            new_center + new_width / 2,
        )

    PLOT_STATE["window_seconds"] = seconds
    axes[0].figure.canvas.draw_idle()
    axes[0].figure.canvas.flush_events()


def zoom_in(event=None):
    """Show a narrower time window around the current view."""

    axes = PLOT_STATE["axes"]

    if len(axes) == 0:
        return

    set_plot_window(max(PLOT_STATE["window_seconds"] * 0.5, 1))
    print("[PLOT] Zoom: IN")


def zoom_out(event=None):
    """Show a wider time window around the current view."""

    axes = PLOT_STATE["axes"]

    if len(axes) == 0:
        return

    set_plot_window(min(PLOT_STATE["window_seconds"] * 2, PLOT_HISTORY_SECONDS))
    print("[PLOT] Zoom: OUT")


def toggle_follow(event=None):
    set_auto_follow(not PLOT_STATE["auto_follow"])


def handle_plot_key(event):
    """Handle the keyboard shortcut for resuming the live plot view."""

    if event.key and event.key.lower() == "f":
        toggle_follow(event)

    elif event.key and event.key.lower() == "h":
        reset_plot_view(event)

    elif event.key and event.key.lower() == "z":
        zoom_in(event)

    elif event.key and event.key.lower() == "p":
        zoom_out(event)


def handle_plot_press(event):
    """Stop auto-follow as soon as a toolbar zoom or pan starts."""

    toolbar = get_plot_toolbar(event)

    if toolbar is not None and toolbar.mode:
        set_auto_follow(False)


def handle_plot_release(event):
    """Stop auto-follow after a toolbar zoom or pan operation."""

    toolbar = get_plot_toolbar(event)

    if toolbar is not None and toolbar.mode:
        set_auto_follow(False)


def configure_hover(figure, lines, labels, units):
    """Attach hover annotations to the current plot lines."""

    for cursor in PLOT_STATE["cursors"]:
        cursor.remove()

    PLOT_STATE["cursors"] = []

    for line, label, unit in zip(lines, labels, units):
        cursor = mplcursors.cursor(line, hover=True)

        @cursor.connect("add")
        def on_add(selection, label=label, unit=unit):
            x_value, y_value = selection.target
            timestamp = mdates.num2date(x_value, tz=LOCAL_TIMEZONE)
            selection.annotation.set_text(
                f"{timestamp:%H:%M:%S}\n{label}: {y_value:.3f} {unit}"
            )

        PLOT_STATE["cursors"].append(cursor)

    if not lines:
        print("[PLOT] WARNING: Hover inspection has no plotted data yet.")


def initialize_plot_lines(figure, axes):
    """Create persistent artists once; live updates only replace their data."""

    for axis in axes:
        axis.grid(True)
        axis.xaxis.set_major_locator(
            mdates.AutoDateLocator(minticks=5, maxticks=9)
        )
        axis.xaxis.set_major_formatter(
            mdates.DateFormatter("%H:%M:%S", tz=LOCAL_TIMEZONE)
        )
        axis.tick_params(axis="x", rotation=0)

    axes[0].set_ylabel("Voltage (V)")
    axes[0].yaxis.set_major_formatter(FormatStrFormatter("%.4f"))
    axes[1].set_ylabel("Current (mA)")
    axes[1].yaxis.set_major_formatter(FormatStrFormatter("%.2f"))
    axes[2].set_ylabel("Power (mW)")
    axes[2].yaxis.set_major_formatter(FormatStrFormatter("%.2f"))
    axes[3].set_ylabel("Accumulated charge (mAh)")
    axes[3].yaxis.set_major_formatter(FormatStrFormatter("%.2f"))
    axes[3].set_xlabel("Local time")

    PLOT_STATE["axes"] = axes
    PLOT_STATE["lines"] = [
        axes[0].plot([], [])[0],
        axes[1].plot([], [])[0],
        axes[2].plot([], [])[0],
        axes[3].plot([], [])[0],
    ]
    configure_hover(
        figure,
        PLOT_STATE["lines"],
        ["Voltage", "Current", "Power", "Charge"],
        ["V", "mA", "mW", "mAh"],
    )


def update_graphs(figure, axes, samples, charge_intervals):
    """
    Redraw the live charts using collected high-resolution samples.
    """

    if not samples and not charge_intervals:
        return

    preserved_limits = None

    if not PLOT_STATE["auto_follow"]:
        preserved_limits = [
            (axis.get_xlim(), axis.get_ylim())
            for axis in axes
        ]

    sample_times = [
        math.nan
        if row.get("plot_gap")
        else mdates.date2num(parse_captured_at(row["captured_at"]))
        for row in samples
    ]

    charge_times = [
        math.nan
        if row.get("plot_gap")
        else mdates.date2num(parse_captured_at(row["captured_at"]))
        for row in charge_intervals
    ]

    voltages = [
        row["voltage_V"]
        for row in samples
    ]

    currents = [
        row["current_mA"]
        for row in samples
    ]

    powers = [
        row["power_mW"]
        for row in samples
    ]

    accumulated_charge = [
        row["running_charge_mAh"]
        for row in charge_intervals
    ]

    voltage_line, current_line, power_line, charge_line = PLOT_STATE["lines"]
    voltage_line.set_data(sample_times, voltages)
    current_line.set_data(sample_times, currents)
    power_line.set_data(sample_times, powers)
    charge_line.set_data(charge_times, accumulated_charge)

    if PLOT_STATE["auto_follow"]:
        all_times = [
            value
            for value in sample_times + charge_times
            if not math.isnan(value)
        ]

        if all_times:
            newest_time = max(all_times)
            oldest_time = (
                newest_time
                - PLOT_STATE["window_seconds"] / 86400
            )

            for axis in axes:
                axis.set_xlim(oldest_time, newest_time)

        for axis in axes:
            axis.relim()
            axis.autoscale_view(scalex=False, scaley=True)
    else:
        for axis, (x_limits, y_limits) in zip(axes, preserved_limits):
            axis.set_xlim(x_limits)
            axis.set_ylim(y_limits)

    experiment_id = (
        samples[-1]["experiment_id"]
        if samples
        else charge_intervals[-1]["experiment_id"]
    )

    figure.suptitle(
        f"BMW Solar Logger - Experiment {experiment_id}"
    )

    figure.canvas.draw_idle()
    figure.canvas.flush_events()

    # draw_idle schedules the repaint without blocking telemetry capture. The
    # main loop continues to call flush_events while waiting for Serial data.


def trim_plot_history(samples):
    """Keep only the rolling display window and meaningful gap markers."""

    timestamped_samples = [
        row
        for row in samples
        if not row.get("plot_gap")
    ]

    if not timestamped_samples:
        return []

    newest_time = max(
        parse_captured_at(row["captured_at"])
        for row in timestamped_samples
    )
    cutoff = newest_time.timestamp() - PLOT_HISTORY_SECONDS
    retained_samples = [
        row
        for row in timestamped_samples
        if parse_captured_at(row["captured_at"]).timestamp() >= cutoff
    ]

    retained_ids = {id(row) for row in retained_samples}
    trimmed = []

    for index, row in enumerate(samples):
        if not row.get("plot_gap"):
            if id(row) in retained_ids:
                trimmed.append(row)
            continue

        has_retained_before = any(
            id(previous) in retained_ids
            for previous in samples[:index]
            if not previous.get("plot_gap")
        )
        has_retained_after = any(
            id(next_row) in retained_ids
            for next_row in samples[index + 1:]
            if not next_row.get("plot_gap")
        )

        if has_retained_before and has_retained_after:
            trimmed.append(row)

    return trimmed


def insert_plot_discontinuity(samples, charge_intervals):
    """Insert one display-only gap marker into both chart data sets."""

    if (
        (samples and samples[-1].get("plot_gap"))
        or (charge_intervals and charge_intervals[-1].get("plot_gap"))
    ):
        return False

    if not samples and not charge_intervals:
        return False

    # NaN is understood by matplotlib as a missing point, so the line is
    # broken instead of connecting the samples on either side. This marker is
    # display metadata only and must never be written to telemetry CSV files.
    if samples:
        samples.append({
            "plot_gap": True,
            "elapsed_seconds": math.nan,
            "voltage_V": math.nan,
            "current_mA": math.nan,
            "power_mW": math.nan,
            "experiment_id": samples[-1]["experiment_id"],
        })

    if charge_intervals:
        charge_intervals.append({
            "plot_gap": True,
            "captured_at": None,
            "running_charge_mAh": math.nan,
            "experiment_id": charge_intervals[-1]["experiment_id"],
        })
    print("[PLOT] Serial interruption: inserting graph discontinuity.")
    return True


# ============================================================================
# MAIN LOGGER
# ============================================================================

def main():
    print()
    print("============================================================")
    print("BMW SOLAR LOGGER")
    print("============================================================")
    print()

    print(f"[PATH] Project root: {PROJECT_ROOT}")
    print(f"[PATH] Data directory: {DATA_DIR}")
    print(f"[FILE] Samples:   {SAMPLES_FILE}")
    print(f"[FILE] Intervals: {INTERVALS_FILE}")
    print(f"[FILE] Events:    {EVENTS_FILE}")
    print()

    DATA_DIR.mkdir(
        parents=True,
        exist_ok=True,
    )


    # ------------------------------------------------------------------------
    # Open durable CSV logs
    # ------------------------------------------------------------------------

    samples_handle, samples_writer = open_csv_file(
        SAMPLES_FILE,
        SAMPLE_COLUMNS,
    )

    intervals_handle, intervals_writer = open_csv_file(
        INTERVALS_FILE,
        INTERVAL_COLUMNS,
    )

    events_handle, events_writer = open_csv_file(
        EVENTS_FILE,
        EVENT_COLUMNS,
    )

    live_samples = load_recent_samples(SAMPLES_FILE)
    current_experiment_id = (
        live_samples[-1]["experiment_id"]
        if live_samples
        else None
    )
    live_charge_intervals = load_recent_intervals(
        INTERVALS_FILE,
        current_experiment_id,
    )

    if current_experiment_id is None and live_charge_intervals:
        current_experiment_id = live_charge_intervals[-1]["experiment_id"]

    historical_times = [
        row["captured_at"]
        for row in live_samples + live_charge_intervals
        if not row.get("plot_gap")
    ]

    print("[PLOT] Loading historical telemetry...")
    print(f"[PLOT] Loaded {len(live_samples)} sample rows.")
    print(f"[PLOT] Loaded {len(live_charge_intervals)} interval rows.")

    if historical_times:
        history_start = min(historical_times)
        history_end = max(historical_times)
        print(
            f"[PLOT] History range: {history_start:%H:%M:%S} -> "
            f"{history_end:%H:%M:%S}"
        )

        newest_history = max(historical_times)
        history_age = datetime.now(tz=newest_history.tzinfo) - newest_history

        if history_age.total_seconds() > PLOT_HISTORY_SECONDS:
            print(
                "[PLOT] WARNING: Historical telemetry is stale; live data "
                "will replace it as it arrives."
            )

        print("[PLOT] Loaded historical telemetry: ALL OK")
    else:
        print(
            "[PLOT] No valid historical telemetry loaded; waiting for live "
            "data."
        )


    # ------------------------------------------------------------------------
    # Create graph
    # ------------------------------------------------------------------------

    figure, axes = create_graphs()

    if live_samples:
        update_graphs(figure, axes, live_samples, live_charge_intervals)
    elif live_charge_intervals:
        update_graphs(figure, axes, [], live_charge_intervals)

    plot_gap_inserted = False


    # ------------------------------------------------------------------------
    # Serial connection state
    # ------------------------------------------------------------------------

    ser = None
    sent_command_pending_cleanup = None
    serial_opened_at = None
    last_serial_line_at = None
    silent_warning_printed = False
    upload_release_acknowledged = False

    print()
    print("[LOGGER] Starting serial connection manager.")
    print("[LOGGER] Press Control-C to stop.")
    print()


    try:
        while True:

            # --------------------------------------------------------------
            # Upload handshake state
            # --------------------------------------------------------------
            #
            # upload.sh writes REQUESTED before it touches the board. We
            # close our port and write RELEASED as an explicit acknowledgment
            # rather than relying on a sleep and hoping the port is free.
            # While RELEASED remains in the file, Python must stay
            # disconnected: esptool owns the serial device during upload.
            # When upload.sh removes the file, normal discovery resumes.
            upload_state = read_upload_handshake()

            if upload_state == "REQUESTED":
                if not upload_release_acknowledged:
                    print("[SERIAL] Upload REQUESTED. Releasing ESP32 port...")

                    if SERIAL_COMMAND_FILE.exists():
                        print(
                            "[COMMAND] ERROR: Firmware upload is in progress. "
                            "Command not sent."
                        )
                        remove_pending_command()

                    if ser is not None:
                        if not close_serial_connection(ser):
                            print(
                                "[SERIAL] ERROR: Port release failed; "
                                "RELEASED acknowledgment will not be sent."
                            )
                            plt.pause(0.01)
                            continue

                        if insert_plot_discontinuity(
                            live_samples,
                            live_charge_intervals,
                        ):
                            plot_gap_inserted = True

                        ser = None
                        serial_opened_at = None
                        last_serial_line_at = None
                        silent_warning_printed = False
                        print("[SERIAL] Port closed for firmware upload.")
                    else:
                        print("[SERIAL] Port already closed for firmware upload.")

                    try:
                        UPLOAD_HANDSHAKE_FILE.write_text(
                            "RELEASED\n",
                            encoding="utf-8",
                        )

                    except OSError as error:
                        print(
                            "[SERIAL] ERROR: Could not acknowledge upload "
                            f"handshake: {error}"
                        )
                        plt.pause(0.01)
                        continue

                    upload_release_acknowledged = True
                    print("[SERIAL] Upload handshake: RELEASED")

                plt.pause(0.01)
                continue

            if upload_state is not None:
                # This includes RELEASED and the brief empty-file window. A
                # present handshake file always blocks discovery, so Python
                # cannot race esptool for the serial port.
                if upload_state not in ("RELEASED", ""):
                    print(
                        "[SERIAL] WARNING: Unknown upload handshake state: "
                        f"{upload_state!r}. Staying disconnected."
                    )

                if ser is not None:
                    close_serial_connection(ser)
                    ser = None
                    serial_opened_at = None
                    last_serial_line_at = None
                    silent_warning_printed = False

                plt.pause(0.01)
                continue

            if upload_release_acknowledged:
                upload_release_acknowledged = False
                print("[SERIAL] Upload finished. Resuming ESP32 discovery...")

            sent_command_pending_cleanup = send_pending_command(
                ser,
                sent_command_pending_cleanup,
            )

            # ================================================================
            # DISCONNECTED STATE
            # ================================================================
            #
            # If ser is None, we currently have no live Serial connection.
            #
            # This happens:
            #
            #   - when the program first starts
            #   - while firmware is being uploaded
            #   - if the USB cable is disconnected
            #   - if the board resets in a way that removes the USB device
            #
            if ser is None:

                port = find_xiao_port()

                if port is None:
                    print(
                        "[SERIAL] XIAO not found. "
                        "Waiting..."
                    )

                    plt.pause(0.01)
                    time.sleep(RECONNECT_DELAY_SECONDS)
                    continue


                print(f"[SERIAL] Found candidate port: {port}")
                print("[SERIAL] Attempting connection...")


                try:
                    ser = serial.Serial(
                        port=port,
                        baudrate=BAUD_RATE,
                        timeout=SERIAL_TIMEOUT_SECONDS,
                    )

                    print(
                        f"[SERIAL] CONNECTED: {port} "
                        f"at {BAUD_RATE} baud"
                    )
                    print()

                    # time.monotonic() measures elapsed duration without being
                    # affected by a wall-clock adjustment. An open port only
                    # proves that macOS opened a device; it does not prove the
                    # sketch is running and producing firmware telemetry.
                    serial_opened_at = time.monotonic()
                    last_serial_line_at = None
                    silent_warning_printed = False


                except serial.SerialException as error:
                    print(
                        "[SERIAL] WARNING: "
                        f"Could not open {port}: {error}"
                    )

                    ser = None

                    plt.pause(0.01)
                    time.sleep(RECONNECT_DELAY_SECONDS)

                    continue


            # ================================================================
            # CONNECTED STATE
            # ================================================================

            try:
                raw_line = ser.readline()


            except (
                serial.SerialException,
                OSError,
            ) as error:

                print()
                print(
                    "[SERIAL] WARNING: "
                    f"Connection lost: {error}"
                )

                close_serial_connection(ser)
                if insert_plot_discontinuity(
                    live_samples,
                    live_charge_intervals,
                ):
                    plot_gap_inserted = True

                ser = None
                serial_opened_at = None
                last_serial_line_at = None
                silent_warning_printed = False

                print(
                    "[SERIAL] Waiting for ESP32 to reappear..."
                )
                print()

                continue


            # A timeout with no data is not necessarily an error.
            if not raw_line:
                now = time.monotonic()
                seconds_since_line = (
                    now - serial_opened_at
                    if last_serial_line_at is None
                    else now - last_serial_line_at
                )

                if (
                    seconds_since_line >= SILENT_WARNING_SECONDS
                    and not silent_warning_printed
                ):
                    print(
                        "[SERIAL] WARNING: Port is open but no firmware data "
                        f"has arrived for {seconds_since_line:.1f} seconds."
                    )
                    print("[SERIAL] State: CONNECTED-BUT-SILENT")
                    print(
                        "[SERIAL] The ESP32 may be in the ROM downloader, "
                        "reset state, or otherwise not running the sketch."
                    )
                    silent_warning_printed = True

                if seconds_since_line >= SILENT_DISCONNECT_SECONDS:
                    print(
                        "[SERIAL] ERROR: No firmware data arrived for "
                        f"{seconds_since_line:.1f} seconds. Closing port "
                        "and retrying discovery."
                    )
                    close_serial_connection(ser)
                    if insert_plot_discontinuity(
                        live_samples,
                        live_charge_intervals,
                    ):
                        plot_gap_inserted = True

                    ser = None
                    serial_opened_at = None
                    last_serial_line_at = None
                    silent_warning_printed = False

                plt.pause(0.01)
                continue

            now = time.monotonic()
            was_silent = silent_warning_printed
            last_serial_line_at = now

            if was_silent:
                print("[SERIAL] Firmware data resumed: ALL OK")
                silent_warning_printed = False


            line = raw_line.decode(
                "utf-8",
                errors="replace",
            ).strip()


            if not line:
                continue


            # ================================================================
            # ALWAYS PRINT FIRMWARE OUTPUT
            # ================================================================
            #
            # Parsing telemetry must never hide diagnostics.
            #
            print(line)


            # ================================================================
            # MESSAGE DISPATCH
            # ================================================================
            #
            # This is the important new part.
            #
            # Identify WHAT kind of message we received before trying to parse
            # its fields.
            #
            if line.startswith("CSV_SAMPLE,"):

                sample = parse_sample(line)

                if sample is None:
                    continue

                sample = {
                    "captured_at": current_host_timestamp(),
                    **sample,
                }

                if plot_gap_inserted:
                    plot_gap_inserted = False

                samples_writer.writerow(sample)
                samples_handle.flush()


                # ------------------------------------------------------------
                # Experiment boundary
                # ------------------------------------------------------------

                if (
                    current_experiment_id
                    != sample["experiment_id"]
                ):

                    print(
                        "[LOGGER] Live experiment changed: "
                        f"{current_experiment_id} -> "
                        f"{sample['experiment_id']}"
                    )

                    current_experiment_id = (
                        sample["experiment_id"]
                    )

                    # We do not want experiment 1 and experiment 2 joined by a
                    # misleading line on the same graph.
                    live_samples = []
                    live_charge_intervals = []
                    plot_gap_inserted = False


                live_samples.append(sample)
                live_samples = trim_plot_history(live_samples)


                update_graphs(
                    figure,
                    axes,
                    live_samples,
                    live_charge_intervals,
                )


            elif line.startswith("CSV_DATA,"):

                interval = parse_interval(line)

                if interval is None:
                    continue

                interval = {
                    "captured_at": current_host_timestamp(),
                    **interval,
                }

                if current_experiment_id != interval["experiment_id"]:
                    print(
                        "[LOGGER] Live experiment changed from interval data: "
                        f"{current_experiment_id} -> "
                        f"{interval['experiment_id']}"
                    )
                    current_experiment_id = interval["experiment_id"]
                    live_samples = []
                    live_charge_intervals = []
                    plot_gap_inserted = False

                # Use the firmware's completed-interval running total rather
                # than integrating one-second samples again on the host. This
                # keeps the chart aligned with canonical INA228 accounting.
                live_charge_intervals.append(interval)
                live_charge_intervals = trim_plot_history(live_charge_intervals)

                intervals_writer.writerow(interval)
                intervals_handle.flush()

                print(
                    "[CAPTURE] Saved interval "
                    f"{interval['experiment_id']}:"
                    f"{interval['interval']}"
                )

                update_graphs(
                    figure,
                    axes,
                    live_samples,
                    live_charge_intervals,
                )


            elif line.startswith("CSV_EVENT,"):

                event = parse_event(line)

                if event is None:
                    continue

                # CSV_EVENT has a variable-length tail. parse_event preserves
                # every field after experiment_id in one comma-joined detail
                # value so no event information is silently discarded.
                event = {
                    "captured_at": current_host_timestamp(),
                    **event,
                }

                events_writer.writerow(event)
                events_handle.flush()

                print(
                    "[CAPTURE] Saved event "
                    f"{event['event_type']} for experiment "
                    f"{event['experiment_id']}"
                )


            else:

                # Ordinary firmware diagnostics need no additional processing.
                #
                # They were already printed above.
                pass


    except KeyboardInterrupt:

        print()
        print()
        print("[LOGGER] Control-C received.")
        print("[LOGGER] Stopping...")


    finally:

        if ser is not None:
            if close_serial_connection(ser):
                print("[SERIAL] Serial connection closed.")


        samples_handle.close()
        intervals_handle.close()
        events_handle.close()

        print("[FILE] CSV logs closed.")
        print("[LOGGER] Shutdown complete.")


if __name__ == "__main__":
    main()