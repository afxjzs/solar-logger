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
        3,
        1,
        figsize=(11, 9),
        sharex=True,
    )

    figure.suptitle("BMW Solar Logger")

    return figure, axes


def update_graphs(figure, axes, samples):
    """
    Redraw the live charts using collected high-resolution samples.
    """

    if not samples:
        return

    times = [
        row["elapsed_seconds"]
        for row in samples
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

    for axis in axes:
        axis.clear()
        axis.grid(True)

    axes[0].plot(times, voltages)
    axes[0].set_ylabel("Voltage (V)")

    axes[1].plot(times, currents)
    axes[1].set_ylabel("Current (mA)")

    axes[2].plot(times, powers)
    axes[2].set_ylabel("Power (mW)")
    axes[2].set_xlabel("Experiment elapsed time (seconds)")

    experiment_id = samples[-1]["experiment_id"]

    figure.suptitle(
        f"BMW Solar Logger - Experiment {experiment_id}"
    )

    figure.tight_layout()

    figure.canvas.draw_idle()
    figure.canvas.flush_events()

    # Without this tiny pause, matplotlib's GUI event loop does not always get
    # enough time to repaint the window.
    plt.pause(0.01)


def insert_plot_discontinuity(samples):
    """Insert one display-only NaN row so matplotlib breaks every line."""

    if not samples or samples[-1].get("plot_gap"):
        return False

    # NaN is understood by matplotlib as a missing point, so the line is
    # broken instead of connecting the samples on either side. This marker is
    # display metadata only and must never be written to telemetry CSV files.
    samples.append({
        "plot_gap": True,
        "elapsed_seconds": math.nan,
        "voltage_V": math.nan,
        "current_mA": math.nan,
        "power_mW": math.nan,
        "experiment_id": samples[-1]["experiment_id"],
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


    # ------------------------------------------------------------------------
    # Create graph
    # ------------------------------------------------------------------------

    figure, axes = create_graphs()

    live_samples = []
    plot_gap_inserted = False

    current_experiment_id = None


    # ------------------------------------------------------------------------
    # Serial connection state
    # ------------------------------------------------------------------------

    ser = None
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

                    if ser is not None:
                        if not close_serial_connection(ser):
                            print(
                                "[SERIAL] ERROR: Port release failed; "
                                "RELEASED acknowledgment will not be sent."
                            )
                            plt.pause(0.01)
                            continue

                        if insert_plot_discontinuity(live_samples):
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
                if insert_plot_discontinuity(live_samples):
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
                    if insert_plot_discontinuity(live_samples):
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
                    plot_gap_inserted = False


                live_samples.append(sample)


                update_graphs(
                    figure,
                    axes,
                    live_samples,
                )


            elif line.startswith("CSV_DATA,"):

                interval = parse_interval(line)

                if interval is None:
                    continue

                interval = {
                    "captured_at": current_host_timestamp(),
                    **interval,
                }

                intervals_writer.writerow(interval)
                intervals_handle.flush()

                print(
                    "[CAPTURE] Saved interval "
                    f"{interval['experiment_id']}:"
                    f"{interval['interval']}"
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