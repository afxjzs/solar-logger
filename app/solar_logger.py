#!/usr/bin/env python3

"""
BMW Solar Logger - Mac Serial Capture + Live Graphs

What this program does:

1. Finds serial devices connected to the Mac.
2. Opens the ESP32 serial connection at 115200 baud.
3. Prints ALL incoming Serial output to the terminal.
4. Looks specifically for lines beginning with:

       CSV_DATA,

5. Parses those machine-readable rows.
6. Saves every row into a CSV file on the Mac.
7. Displays live graphs for:
       - battery voltage
       - solar current
       - solar power
       - cumulative solar energy

IMPORTANT:
The Arduino Serial Monitor must be CLOSED while this program is running.

Only one program can normally own the ESP32's serial port at a time.
"""

import csv
import sys
from pathlib import Path

import matplotlib.pyplot as plt
import serial
from serial.tools import list_ports


# ---------------------------------------------------------------------------
# SERIAL CONFIGURATION
# ---------------------------------------------------------------------------

BAUD_RATE = 115200


# These are the columns emitted by the ESP32 firmware.
#
# The first item on the Serial line is literally "CSV_DATA", so that marker
# is not included in this list.
CSV_COLUMNS = [
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


# ---------------------------------------------------------------------------
# FIND THE ESP32 SERIAL PORT
# ---------------------------------------------------------------------------

def choose_serial_port():
    """
    Ask pyserial for all serial ports currently visible to macOS.

    Typical ESP32/XIAO names on a Mac look something like:

        /dev/cu.usbmodem1101
        /dev/cu.usbserial-XXXX

    We print everything we find rather than silently guessing.
    """

    ports = list(list_ports.comports())

    if not ports:
        print("[ERROR] No serial ports found.")
        print("[ERROR] Is the XIAO connected by USB?")
        sys.exit(1)

    print()
    print("Serial ports found:")
    print()

    for index, port in enumerate(ports):
        print(f"  [{index}] {port.device}")
        print(f"      description: {port.description}")
        print(f"      manufacturer: {port.manufacturer}")
        print()

    # If there is only one serial port, choosing it automatically is
    # unambiguous.
    if len(ports) == 1:
        selected = ports[0]

        print(f"Using the only available port: {selected.device}")
        return selected.device

    # Otherwise let the user explicitly choose.
    while True:
        choice = input("Choose the ESP32 serial-port number: ").strip()

        try:
            index = int(choice)

            if 0 <= index < len(ports):
                return ports[index].device

        except ValueError:
            pass

        print("Invalid selection. Try again.")


# ---------------------------------------------------------------------------
# PARSE ONE CSV_DATA LINE
# ---------------------------------------------------------------------------

def parse_csv_data(line):
    """
    Convert a firmware CSV_DATA line into normal Python values.

    Example input:

        CSV_DATA,1,3,60.000,12.83,272.4,...

    line.split(",") gives us a Python list:

        [
            "CSV_DATA",
            "1",
            "3",
            "60.000",
            "12.83",
            ...
        ]

    We discard "CSV_DATA" and map the remaining fields to CSV_COLUMNS.
    """

    parts = line.split(",")

    expected_parts = len(CSV_COLUMNS) + 1

    if len(parts) != expected_parts:
        print(
            f"[WARNING] CSV_DATA field count is wrong. "
            f"Expected {expected_parts}, got {len(parts)}"
        )

        return None

    values = parts[1:]

    row = dict(zip(CSV_COLUMNS, values))

    # Convert fields into numeric Python values.
    #
    # experiment_id and interval are whole numbers.
    row["experiment_id"] = int(row["experiment_id"])
    row["interval"] = int(row["interval"])

    # Everything else is a decimal measurement.
    for column in CSV_COLUMNS[2:]:
        row[column] = float(row[column])

    return row


# ---------------------------------------------------------------------------
# LIVE GRAPH SETUP
# ---------------------------------------------------------------------------

def create_graphs():
    """
    Create the live dashboard window.

    We keep four separate axes because voltage, current, power, and energy use
    very different units and scales.
    """

    plt.ion()

    figure, axes = plt.subplots(
        4,
        1,
        figsize=(11, 10),
        sharex=True,
    )

    figure.suptitle("BMW Solar Logger")

    axes[0].set_ylabel("Voltage (V)")
    axes[1].set_ylabel("Current (mA)")
    axes[2].set_ylabel("Power (mW)")
    axes[3].set_ylabel("Energy (mWh)")
    axes[3].set_xlabel("Interval")

    axes[0].grid(True)
    axes[1].grid(True)
    axes[2].grid(True)
    axes[3].grid(True)

    return figure, axes


# ---------------------------------------------------------------------------
# REDRAW LIVE GRAPH
# ---------------------------------------------------------------------------

def update_graphs(figure, axes, rows):
    """
    Redraw the plots using every CSV_DATA row collected for the current
    experiment.

    This is deliberately simple for now.

    One minute of data creates only one point, so even many hours of bench
    testing is a very small dataset for a laptop.
    """

    if not rows:
        return

    experiment_id = rows[-1]["experiment_id"]

    intervals = [row["interval"] for row in rows]
    voltage = [row["voltage_V"] for row in rows]
    current = [row["current_mA"] for row in rows]
    power = [row["power_mW"] for row in rows]
    energy = [row["running_energy_mWh"] for row in rows]

    for axis in axes:
        axis.clear()

    axes[0].plot(intervals, voltage)
    axes[0].set_ylabel("Voltage (V)")
    axes[0].grid(True)

    axes[1].plot(intervals, current)
    axes[1].set_ylabel("Current (mA)")
    axes[1].grid(True)

    axes[2].plot(intervals, power)
    axes[2].set_ylabel("Power (mW)")
    axes[2].grid(True)

    axes[3].plot(intervals, energy)
    axes[3].set_ylabel("Energy (mWh)")
    axes[3].set_xlabel("Interval (minutes)")
    axes[3].grid(True)

    figure.suptitle(
        f"BMW Solar Logger - Experiment {experiment_id}"
    )

    figure.tight_layout()

    # Tell matplotlib to actually refresh the window.
    figure.canvas.draw_idle()
    figure.canvas.flush_events()

    plt.pause(0.01)


# ---------------------------------------------------------------------------
# MAIN PROGRAM
# ---------------------------------------------------------------------------

def main():
    port = choose_serial_port()

    print()
    print("============================================================")
    print("BMW SOLAR LOGGER - MAC CAPTURE")
    print("============================================================")
    print()
    print(f"Serial port: {port}")
    print(f"Baud rate:   {BAUD_RATE}")
    print()

    # Open the serial connection.
    #
    # timeout=1 means readline() will wait at most one second for data instead
    # of hanging forever.
    try:
        ser = serial.Serial(
            port=port,
            baudrate=BAUD_RATE,
            timeout=1,
        )

    except serial.SerialException as error:
        print(f"[ERROR] Could not open serial port: {error}")
        sys.exit(1)

    print("[SERIAL] Port opened successfully.")
    print()


    # -----------------------------------------------------------------------
    # OUTPUT FILE
    # -----------------------------------------------------------------------

    output_path = Path("bmw_solar_log.csv")

    file_exists = output_path.exists()

    csv_file = output_path.open(
        "a",
        newline="",
        buffering=1,
    )

    writer = csv.DictWriter(
        csv_file,
        fieldnames=CSV_COLUMNS,
    )

    # Write column names if this is a new file.
    if not file_exists:
        writer.writeheader()

        print(
            f"[FILE] Created new log file: "
            f"{output_path.resolve()}"
        )
    else:
        print(
            f"[FILE] Appending to existing log file: "
            f"{output_path.resolve()}"
        )


    # -----------------------------------------------------------------------
    # LIVE GRAPH
    # -----------------------------------------------------------------------

    figure, axes = create_graphs()


    # Rows currently being displayed.
    #
    # If we encounter a new experiment ID, we'll clear this list so the live
    # chart automatically switches to the new experiment.
    graph_rows = []

    current_experiment_id = None


    print()
    print("[LOGGER] Listening for ESP32 Serial data...")
    print("[LOGGER] Press Control-C to stop.")
    print()


    try:
        while True:

            # readline() waits until the ESP32 sends a newline.
            #
            # Serial data arrives as bytes, so decode() converts those bytes
            # into normal Python text.
            raw_line = ser.readline()

            if not raw_line:
                # Nothing arrived during this one-second timeout.
                #
                # Keep matplotlib responsive anyway.
                plt.pause(0.01)
                continue


            try:
                line = raw_line.decode(
                    "utf-8",
                    errors="replace",
                ).strip()

            except Exception as error:
                print(
                    f"[ERROR] Could not decode Serial line: {error}"
                )
                continue


            if not line:
                continue


            # Print EVERYTHING from the firmware.
            #
            # We want ALL OK, warnings, NVS messages, heartbeats, errors, etc.
            print(line)


            # Ordinary diagnostic lines require no parsing.
            if not line.startswith("CSV_DATA,"):
                continue


            try:
                row = parse_csv_data(line)

            except Exception as error:
                print(
                    f"[ERROR] Could not parse CSV_DATA line: {error}"
                )
                continue


            if row is None:
                continue


            # Immediately save the data point to disk.
            writer.writerow(row)

            print(
                "[CAPTURE] Saved "
                f"experiment {row['experiment_id']} "
                f"interval {row['interval']}"
            )


            # ---------------------------------------------------------------
            # Handle experiment boundaries.
            # ---------------------------------------------------------------

            if current_experiment_id != row["experiment_id"]:

                print(
                    "[CAPTURE] Experiment changed: "
                    f"{current_experiment_id} -> "
                    f"{row['experiment_id']}"
                )

                current_experiment_id = row["experiment_id"]

                # Start a fresh live graph for the new experiment.
                graph_rows = []


            graph_rows.append(row)

            update_graphs(
                figure,
                axes,
                graph_rows,
            )


    except KeyboardInterrupt:
        print()
        print()
        print("[LOGGER] Control-C received.")
        print("[LOGGER] Stopping capture...")


    finally:
        csv_file.close()
        ser.close()

        print("[FILE] CSV file closed.")
        print("[SERIAL] Serial port closed.")
        print("[LOGGER] Shutdown complete.")


if __name__ == "__main__":
    main()