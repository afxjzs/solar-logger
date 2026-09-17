#!/usr/bin/env python3

"""
Archive the three host CSV logs and start fresh empty ones.

    data/samples.csv     ->  logs/<datetime>-exp<N>/samples.csv
    data/intervals.csv   ->  logs/<datetime>-exp<N>/intervals.csv
    data/events.csv      ->  logs/<datetime>-exp<N>/events.csv

New empty files are then created in data/, each carrying the same header its
archived file had.

THIS SCRIPT NEVER TOUCHES THE DEVICE.

It opens no serial port, sends no command, and changes nothing in NVS. The
ESP32's experiment id, interval number, running totals, and autonomous storage
are all untouched. Archiving is a host-side file operation, and the next row the
firmware sends continues the same experiment into the new files.

WHY IT REFUSES TO RUN WHILE THE LOGGER IS RUNNING

app/solar_logger.py opens all three files in append mode and holds the handles
open for its entire run. Moving a file out from under an open handle does not
redirect that handle: the logger would keep writing into the archived file
through the same inode while the new file in data/ stayed empty. Truncating in
place is no better, because the handle keeps its offset and the next append
lands past it, leaving a hole of NUL bytes.

Either way the data goes somewhere the operator is not looking, and nothing
errors. So this refuses to run rather than producing a plausible-looking result.

HEADERS ARE COPIED, NOT HARDCODED

open_csv_file() in app/solar_logger.py compares an existing file's header
against its expected columns and exits if they differ. Copying each header from
the file being archived guarantees the match and keeps working when the columns
change. A hardcoded copy here would be a second source of truth that silently
drifts.

Usage:

    tools/archive-data.py
    tools/archive-data.py --dry-run
    tools/archive-data.py --exp 3
"""

import argparse
import csv
import subprocess
import sys
from datetime import datetime
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DATA_DIR = REPO_ROOT / "data"
LOGS_DIR = REPO_ROOT / "logs"

# Order matters only for readable output.
CSV_NAMES = ["samples.csv", "intervals.csv", "events.csv"]

# Every one of these files carries the experiment id under this column name.
# Looked up by name rather than position so a column insertion cannot silently
# make this read the wrong field.
EXPERIMENT_COLUMN = "experiment_id"

LOGGER_PROCESS_PATTERN = r"(^|[[:space:]/])app/solar_logger\.py([[:space:]]|$)"


def logger_is_running():
    """True when app/solar_logger.py currently owns the CSV handles.

    Uses the same pattern as tools/send.sh so the two tools always agree about
    whether the logger is running.
    """

    result = subprocess.run(
        ["pgrep", "-f", LOGGER_PROCESS_PATTERN],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )

    return result.returncode == 0


def read_csv_summary(path):
    """Read one CSV's header, row count, experiment id, and time span.

    Returns None when the file does not exist. Raises on a malformed file
    rather than guessing at its contents.
    """

    if not path.exists():
        return None

    with path.open("r", newline="") as handle:
        reader = csv.reader(handle)

        header = next(reader, None)

        if header is None:
            raise ValueError(f"{path} is completely empty; it has no header row")

        rows = 0
        last_row = None
        first_row = None

        for row in reader:
            # A trailing blank line is normal at the end of a text file.
            if not row:
                continue

            if first_row is None:
                first_row = row

            last_row = row
            rows += 1

    summary = {
        "path": path,
        "header": header,
        "rows": rows,
        "experiment_id": None,
        "first_captured_at": None,
        "last_captured_at": None,
    }

    if last_row is not None:
        if EXPERIMENT_COLUMN in header:
            index = header.index(EXPERIMENT_COLUMN)

            if index < len(last_row):
                summary["experiment_id"] = last_row[index]
        else:
            # Never silently skip it: a file without this column breaks the
            # directory-naming assumption and the operator needs to know.
            print(
                f"[ARCHIVE] WARNING: {path.name} has no {EXPERIMENT_COLUMN} "
                f"column. It cannot contribute an experiment number."
            )

        if "captured_at" in header:
            index = header.index("captured_at")

            if first_row is not None and index < len(first_row):
                summary["first_captured_at"] = first_row[index]

            if index < len(last_row):
                summary["last_captured_at"] = last_row[index]

    return summary


def resolve_experiment_id(summaries, override):
    """Decide which experiment number names the archive directory.

    Returns (value, explanation). Never picks silently when the files
    disagree: it says what it saw and why it chose what it chose.
    """

    if override is not None:
        return str(override), "given on the command line with --exp"

    observed = {}

    for summary in summaries:
        if summary is None or summary["experiment_id"] is None:
            continue

        observed[summary["path"].name] = summary["experiment_id"]

    if not observed:
        return None, "no file carried an experiment id"

    distinct = set(observed.values())

    if len(distinct) == 1:
        return next(iter(distinct)), "all files agree"

    # A reset between one file's last row and another's makes this legitimate,
    # so it is reported rather than treated as an error. The highest id is the
    # newest, because experiment ids only ever increment.
    print("[ARCHIVE] NOTE: the files do not all report the same experiment id:")

    for name, value in observed.items():
        print(f"[ARCHIVE]   {name}: {value}")

    def sort_key(value):
        try:
            return (0, int(value))
        except ValueError:
            return (1, 0)

    chosen = max(distinct, key=sort_key)

    return chosen, "highest of the differing values, since ids only increment"


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Archive data/*.csv into logs/<datetime>-exp<N>/ and create new "
            "empty CSVs. Never touches the device."
        )
    )

    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="show exactly what would happen and change nothing",
    )

    parser.add_argument(
        "--exp",
        type=int,
        default=None,
        help="experiment number for the directory name, instead of reading it "
        "from the CSV contents",
    )

    args = parser.parse_args()

    print("[ARCHIVE] BMW Solar Logger CSV archiver")
    print(f"[ARCHIVE] Repository: {REPO_ROOT}")
    print("[ARCHIVE] The device is NOT touched: no serial port, no commands,")
    print("[ARCHIVE] no NVS changes. Host files only.")
    print()

    # ------------------------------------------------------------------------
    # Refuse to race the logger's open append handles.
    # ------------------------------------------------------------------------

    if logger_is_running():
        print("[ARCHIVE] ERROR: app/solar_logger.py is running.", file=sys.stderr)
        print(
            "[ARCHIVE] It holds all three CSV files open in append mode, so "
            "archiving now",
            file=sys.stderr,
        )
        print(
            "[ARCHIVE] would leave it writing into the archived file while the "
            "new one stayed",
            file=sys.stderr,
        )
        print(
            "[ARCHIVE] empty, with nothing reporting an error. Stop the logger "
            "first.",
            file=sys.stderr,
        )
        print(
            "[ARCHIVE] Stopping the logger does not affect the device or the "
            "experiment.",
            file=sys.stderr,
        )
        return 1

    if not DATA_DIR.is_dir():
        print(f"[ARCHIVE] ERROR: {DATA_DIR} does not exist.", file=sys.stderr)
        return 1

    # ------------------------------------------------------------------------
    # Inspect what is there before moving anything.
    # ------------------------------------------------------------------------

    summaries = []

    for name in CSV_NAMES:
        path = DATA_DIR / name

        try:
            summary = read_csv_summary(path)

        except (OSError, ValueError) as error:
            print(f"[ARCHIVE] ERROR: Could not read {path}: {error}", file=sys.stderr)
            return 1

        summaries.append(summary)

        if summary is None:
            print(f"[ARCHIVE] {name}: MISSING, nothing to archive")
            continue

        span = ""

        if summary["first_captured_at"] and summary["last_captured_at"]:
            span = (
                f"  {summary['first_captured_at']} -> "
                f"{summary['last_captured_at']}"
            )

        print(
            f"[ARCHIVE] {name}: {summary['rows']} data rows, "
            f"experiment {summary['experiment_id']}{span}"
        )

    present = [s for s in summaries if s is not None]

    if not present:
        print()
        print("[ARCHIVE] Nothing to do: none of the three CSV files exist.")
        return 0

    if all(s["rows"] == 0 for s in present):
        print()
        print(
            "[ARCHIVE] Nothing to do: every existing CSV holds only a header "
            "row."
        )
        print("[ARCHIVE] No archive directory was created.")
        return 0

    # ------------------------------------------------------------------------
    # Name the archive directory.
    # ------------------------------------------------------------------------

    experiment_id, reason = resolve_experiment_id(summaries, args.exp)

    if experiment_id is None:
        print()
        print(
            "[ARCHIVE] ERROR: Could not determine the experiment number "
            f"({reason}).",
            file=sys.stderr,
        )
        print(
            "[ARCHIVE] Re-run with --exp N to name the archive explicitly.",
            file=sys.stderr,
        )
        return 1

    # Local time, matching the captured_at timestamps in the files themselves.
    # Sorts correctly as text and stays readable.
    stamp = datetime.now().strftime("%Y-%m-%d_%H%M%S")
    archive_dir = LOGS_DIR / f"{stamp}-exp{experiment_id}"

    print()
    print(f"[ARCHIVE] Experiment number: {experiment_id} ({reason})")
    print(f"[ARCHIVE] Destination: {archive_dir}")
    print()

    if archive_dir.exists():
        print(
            f"[ARCHIVE] ERROR: {archive_dir} already exists. Refusing to write "
            "into it.",
            file=sys.stderr,
        )
        return 1

    # ------------------------------------------------------------------------
    # Do it.
    # ------------------------------------------------------------------------

    if args.dry_run:
        print("[ARCHIVE] DRY RUN. Nothing below actually happened.")
        print(f"[ARCHIVE] Would create {archive_dir}")

        for summary in present:
            print(
                f"[ARCHIVE] Would move {summary['path'].name} "
                f"({summary['rows']} rows) into it"
            )
            print(
                f"[ARCHIVE] Would create an empty {summary['path'].name} "
                f"with {len(summary['header'])} header columns"
            )

        print("[ARCHIVE] Would write an ARCHIVE.txt manifest")
        print("[ARCHIVE] Dry run complete. No files changed.")
        return 0

    archive_dir.mkdir(parents=True)
    print(f"[ARCHIVE] Created {archive_dir}")

    moved = []

    for summary in present:
        source = summary["path"]
        destination = archive_dir / source.name

        # rename() within one filesystem is atomic: the file is either fully
        # at the old path or fully at the new one, never half-copied.
        source.rename(destination)

        archived_bytes = destination.stat().st_size

        print(
            f"[ARCHIVE] Moved {source.name} -> {destination} "
            f"({archived_bytes} bytes, {summary['rows']} rows)"
        )

        moved.append((summary, destination))

    # ------------------------------------------------------------------------
    # Recreate empty files, each with the header its predecessor had.
    # ------------------------------------------------------------------------

    for summary, _ in moved:
        fresh = DATA_DIR / summary["path"].name

        with fresh.open("w", newline="") as handle:
            csv.writer(handle).writerow(summary["header"])

        print(
            f"[ARCHIVE] Created empty {fresh.name} with the same "
            f"{len(summary['header'])}-column header"
        )

    # ------------------------------------------------------------------------
    # Manifest, so the archive explains itself later.
    # ------------------------------------------------------------------------

    manifest = archive_dir / "ARCHIVE.txt"

    lines = [
        "BMW Solar Logger CSV archive",
        "",
        f"Archived at:       {datetime.now().isoformat()}",
        f"Experiment number: {experiment_id} ({reason})",
        "",
        "The device was not touched when this archive was made. No serial",
        "port was opened, no command was sent, and no NVS state changed.",
        "The experiment continued uninterrupted into the new data/ files.",
        "",
        "Contents:",
    ]

    for summary, destination in moved:
        lines.append("")
        lines.append(f"  {destination.name}")
        lines.append(f"    data rows:  {summary['rows']}")
        lines.append(f"    experiment: {summary['experiment_id']}")

        if summary["first_captured_at"]:
            lines.append(f"    first row:  {summary['first_captured_at']}")

        if summary["last_captured_at"]:
            lines.append(f"    last row:   {summary['last_captured_at']}")

    manifest.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"[ARCHIVE] Wrote {manifest.name}")

    # ------------------------------------------------------------------------
    # Verify rather than assume.
    # ------------------------------------------------------------------------

    problems = []

    for summary, destination in moved:
        fresh = DATA_DIR / summary["path"].name

        if not destination.exists():
            problems.append(f"{destination} is missing after the move")

        if not fresh.exists():
            problems.append(f"{fresh} was not recreated")
            continue

        with fresh.open("r", newline="") as handle:
            header = next(csv.reader(handle), [])
            remaining = sum(1 for row in csv.reader(handle) if row)

        if header != summary["header"]:
            problems.append(f"{fresh.name} header does not match the archived one")

        if remaining != 0:
            problems.append(f"{fresh.name} is not empty: {remaining} data rows")

    print()

    if problems:
        print("[ARCHIVE] NOT ALL OK - verification found problems:", file=sys.stderr)

        for problem in problems:
            print(f"[ARCHIVE]   {problem}", file=sys.stderr)

        return 1

    print("[ARCHIVE] Verified: archives written, new files empty with matching")
    print("[ARCHIVE] headers, device untouched: ALL OK")
    print()
    print("[ARCHIVE] The next logger run appends to the fresh files and the")
    print("[ARCHIVE] experiment continues where it left off.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
