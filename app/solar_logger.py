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
import sys
import time
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

LOG_DIR = PROJECT_ROOT / "data"

SAMPLES_FILE = LOG_DIR / "samples.csv"
INTERVALS_FILE = LOG_DIR / "intervals.csv"


# ============================================================================
# SERIAL CONFIGURATION
# ============================================================================

BAUD_RATE = 115200

# How long readline() waits before returning when no Serial data arrives.
SERIAL_TIMEOUT_SECONDS = 1

# How long we wait between reconnect attempts after the ESP32 disappears.
RECONNECT_DELAY_SECONDS = 1


# ============================================================================
# CSV SCHEMAS
# ============================================================================

# CSV_SAMPLE firmware format:
#
# CSV_SAMPLE,
# experiment_id,
# elapsed_seconds,
# voltage_V,
# current_mA,
# power_mW,
# temperature_C
#
SAMPLE_COLUMNS = [
    "experiment_id",
    "elapsed_seconds",
    "voltage_V",
    "current_mA",
    "power_mW",
    "temperature_C",
]


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
INTERVAL_COLUMNS = [
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


# ============================================================================
# SERIAL PORT DISCOVERY
# ============================================================================

def find_xiao_port():
    """
    Find the most likely XIAO ESP32-C3 serial port.

    macOS serial devices suitable for initiating a connection normally use
    /dev/cu.* names.

    Our current board appears as:

        /dev/cu.usbmodem1101

    The exact number can change, so we do not want to permanently hard-code
    usbmodem1101.
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

    expected_parts = len(SAMPLE_COLUMNS) + 1

    if len(parts) != expected_parts:
        print(
            "[WARNING] CSV_SAMPLE field count is wrong. "
            f"Expected {expected_parts}, got {len(parts)}"
        )
        return None

    values = parts[1:]

    row = dict(zip(SAMPLE_COLUMNS, values))

    # Experiment IDs are integers.
    row["experiment_id"] = int(row["experiment_id"])

    # All the measurements are floating-point values.
    for column in SAMPLE_COLUMNS[1:]:
        row[column] = float(row[column])

    return row


def parse_interval(line):
    """
    Convert one CSV_DATA interval summary into a Python dictionary.
    """

    parts = line.split(",")

    expected_parts = len(INTERVAL_COLUMNS) + 1

    if len(parts) != expected_parts:
        print(
            "[WARNING] CSV_DATA field count is wrong. "
            f"Expected {expected_parts}, got {len(parts)}"
        )
        return None

    values = parts[1:]

    row = dict(zip(INTERVAL_COLUMNS, values))

    row["experiment_id"] = int(row["experiment_id"])
    row["interval"] = int(row["interval"])

    for column in INTERVAL_COLUMNS[2:]:
        row[column] = float(row[column])

    return row


# ============================================================================
# CSV FILE HANDLING
# ============================================================================

def open_csv_file(path, columns):
    """
    Open a CSV file for append.

    If the file does not already exist, write the column header first.
    """

    file_exists = path.exists()

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
    print(f"[PATH] Log directory: {LOG_DIR}")
    print()

    LOG_DIR.mkdir(
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


    # ------------------------------------------------------------------------
    # Create graph
    # ------------------------------------------------------------------------

    figure, axes = create_graphs()

    live_samples = []

    current_experiment_id = None


    # ------------------------------------------------------------------------
    # Serial connection state
    # ------------------------------------------------------------------------

    ser = None

    print()
    print("[LOGGER] Starting serial connection manager.")
    print("[LOGGER] Press Control-C to stop.")
    print()


    try:
        while True:

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

                try:
                    ser.close()
                except Exception:
                    pass

                ser = None

                print(
                    "[SERIAL] Waiting for ESP32 to reappear..."
                )
                print()

                continue


            # A timeout with no data is not necessarily an error.
            if not raw_line:
                plt.pause(0.01)
                continue


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


                samples_writer.writerow(sample)


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


                intervals_writer.writerow(interval)

                print(
                    "[CAPTURE] Saved interval "
                    f"{interval['experiment_id']}:"
                    f"{interval['interval']}"
                )


            elif line.startswith("CSV_EVENT,"):

                # We print events already.
                #
                # We do not yet persist these separately.
                #
                # Later we may make an experiment metadata log from them.
                pass


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

            try:
                ser.close()
                print("[SERIAL] Serial connection closed.")

            except Exception as error:
                print(
                    "[SERIAL] WARNING while closing: "
                    f"{error}"
                )


        samples_handle.close()
        intervals_handle.close()

        print("[FILE] CSV logs closed.")
        print("[LOGGER] Shutdown complete.")


if __name__ == "__main__":
    main()