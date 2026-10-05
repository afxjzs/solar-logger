"""Validate a captured LOGGER STORAGE DUMP, and optionally compare it to an earlier one.

    uv run python tools/validate-dump.py <dump.txt> [--compare <earlier-dump.txt>] [--experiment N]

Prints a JSON summary on stdout and exits non-zero if the dump is not a
complete, self-consistent capture of a healthy log.

WHAT IT REFUSES TO ACCEPT, and why each one matters:

    a record line that does not parse    a dump whose format moved is not
                                         evidence about the format this checks
    a count summary that disagrees       the board's own tally must match the
                                         lines it printed, or one of them lies
    a gap in the printed indices         a lost or duplicated line means the
                                         capture is partial, not that the log is
    a sequence that does not increase    reuse or reordering breaks D-023
    a foreign experiment id              the comparison would be against a
                                         different run
    a missing BEGIN, END, ACK or RESULT  the capture stopped early, so absence
                                         of damage below proves nothing
    any ERROR / WARNING / INVALID        the board said something was wrong
    more than one capture-stop reason    two stops means two captures merged

`--compare` additionally asserts that every decoded record line in the earlier
dump appears identically, in the same order, at the head of this one. That is
what shows an upload or a recovery preserved history rather than rewrote it.

The comparison is on the decoded text the board printed. Board-reported CRC
validity is not independent binary verification of the stored bytes.
"""

import argparse
import collections
import hashlib
import json
import pathlib
import re
import sys

RECORD_PREFIX = "[STORAGE] #"

RECORD = re.compile(
    r"^\[STORAGE\] #(\d+) seq=(\d+) boot=(\d+) exp=(\d+) .* interval_ms=(\d+) "
    r".*time=(\w+) epoch=(\d+) flags=0x([0-9A-Fa-f]+)\[.*\] crc=0x([0-9A-Fa-f]+)$"
)

ALLOWED_STOPS = (
    "port quiet for 600ms.",
    "transport disappeared after the successful result (expected for RELEASE).",
)

BAD_WORDS = re.compile(r"ERROR|WARNING|INVALID|NOT ALL OK|TRUNCATED")


def fail(message: str) -> None:
    print(f"[VALIDATE] REJECTED: {message}", file=sys.stderr)
    raise SystemExit(1)


def record_lines(path: pathlib.Path) -> list[str]:
    return [x for x in path.read_text(encoding="utf-8").splitlines()
            if x.startswith(RECORD_PREFIX)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=pathlib.Path)
    parser.add_argument("--compare", type=pathlib.Path, default=None)
    parser.add_argument("--experiment", type=int, default=3)
    options = parser.parse_args()

    path: pathlib.Path = options.dump
    text = path.read_text(encoding="utf-8")
    lines = record_lines(path)

    if not lines:
        fail(f"no decoded record lines in {path}")

    fields: list[tuple[str, ...]] = []
    for line in lines:
        match = RECORD.fullmatch(line)
        if match is None:
            fail(f"record line did not parse, refusing to guess:\n{line}")
        else:
            fields.append(match.groups())

    indices = [int(x[0]) for x in fields]
    seqs = [int(x[1]) for x in fields]
    boots = [x[2] for x in fields]
    exps = {x[3] for x in fields}

    summary = re.findall(r"^\[STORAGE\] Records read: (\d+), invalid: (\d+)$",
                         text, re.M)
    if summary != [(str(len(fields)), "0")]:
        fail(f"count summary {summary} disagrees with {len(fields)} printed "
             f"records and zero invalid")

    if indices != list(range(len(fields))):
        fail("printed record indices are not a complete 0..N-1 run; the capture "
             "lost or duplicated a line")

    if not all(a < b for a, b in zip(seqs, seqs[1:])):
        fail("sequence numbers are not strictly increasing; reuse or reordering")

    if exps != {str(options.experiment)}:
        fail(f"expected only experiment {options.experiment}, found {sorted(exps)}")

    if text.count("[STORAGE] --- BEGIN DUMP ---") != 1:
        fail("expected exactly one BEGIN DUMP marker")

    if text.count("[STORAGE] --- END DUMP ---") != 1:
        fail("expected exactly one END DUMP marker; the capture stopped early")

    if "[COMMAND] Firmware parsed command: ALL OK (CMD_ACK,LOGGER STORAGE DUMP)" \
            not in text:
        fail("no matching CMD_ACK for LOGGER STORAGE DUMP")

    if not re.search(r"^CMD_RESULT,LOGGER STORAGE DUMP,OK$", text, re.M):
        fail("no matching CMD_RESULT OK for LOGGER STORAGE DUMP")

    stops = re.findall(r"^\[COMMAND\] Capture ended: (.+)$", text, re.M)
    if len(stops) != 1:
        fail(f"expected exactly one capture-stop reason, found {stops}")
    if stops[0] not in ALLOWED_STOPS:
        fail(f"unexpected capture-stop reason: {stops[0]}")

    flagged = BAD_WORDS.search(text)
    if flagged is not None:
        fail(f"the board reported a problem: {flagged.group(0)!r} appears in the "
             f"capture")

    result: dict[str, object] = {
        "file": path.name,
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "records": len(fields),
        "first_seq": seqs[0],
        "last_seq": seqs[-1],
        "experiment_ids": sorted(exps),
        "sequence_gaps": [(a, b) for a, b in zip(seqs, seqs[1:]) if b != a + 1],
        "board_invalid": 0,
        "complete": True,
        "capture_stop": stops[0],
        "boot_counts": dict(collections.Counter(boots)),
        "time_quality": dict(collections.Counter(x[5] for x in fields)),
        "epochs": sorted({x[6] for x in fields}),
        "last_records": lines[-8:],
    }

    if options.compare is not None:
        earlier = record_lines(options.compare)
        if lines[:len(earlier)] != earlier:
            fail(f"the {len(earlier)} decoded records from {options.compare.name} "
                 f"are NOT identical at the head of this dump; history changed")
        result["compared_against"] = options.compare.name
        result["identical_historical_record_lines"] = len(earlier)

    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
