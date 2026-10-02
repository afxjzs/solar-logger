"""Did the running-total reseed survive the upload's reset? (D-060)

    tools/check-totals-continuity.py <post-dump.txt> --compare <pre-dump.txt>
    tools/check-totals-continuity.py <post-dump.txt>          # boundary inferred

`autoStorageRecover()` reseeds the RTC-retained running totals from the last
valid record in the durable log, so a reboot continues the totals instead of
restarting them at zero. D-060 made that reseed conditional: it happens only
when the scan read the log to its end. A healthy log is always read to its end,
so the reseed must still happen, and the proof is arithmetic rather than a
printed message.

For the first record written after the reset:

    Qsum_uAh(first) == Qsum_uAh(previous) + dQ_uAh(first)
    Esum_uWh(first) == Esum_uWh(previous) + dE_uWh(first)

Measured across boot 36 -> 37 in the 2026-10-01 acceptance of commit 58a4e09:

    seq 12511  Qsum_uAh=1007893  Esum_uWh=16203190
    seq 12512  dQ_uAh=-10  dE_uWh=128  Qsum_uAh=1007883  Esum_uWh=16203318

    1007893 + (-10) = 1007883     16203190 + 128 = 16203318

If that identity fails at the upload's boundary, the reseed did not run and the
totals silently restarted, which is the defect D-060 exists to prevent arriving
from the opposite direction.

PASS `--compare` WITH THE PREFLIGHT DUMP. It is what identifies the right
boundary. Without it this script infers the boundary as the newest boot in the
log, which is only the upload's boot if the dump was taken before the board
rebooted again. That assumption broke in the 58a4e09 acceptance: the dump came
two days after the upload, three further boots had happened, and the inferred
boundary was 39 -> 40 while the upload was 36 -> 37. The check still passed, but
on a boundary that had nothing to do with the commit under test, which is a
result that looks like evidence and is not.

Boundaries other than the upload's are REPORTED, not asserted. A durable log can
span firmware that predates the current recovery code, so a historical break
says nothing about the commit under test. As of 2026-10-01 the Experiment 3 log
had zero breaks across all 13,650 records, so a new one would stand out.

This reads the decoded text the board printed. It is not independent
verification of the stored bytes.
"""

import argparse
import json
import pathlib
import re
import sys

RECORD_PREFIX = "[STORAGE] #"

RECORD = re.compile(
    r"^\[STORAGE\] #(?P<index>\d+) seq=(?P<seq>\d+) boot=(?P<boot>\d+) "
    r"exp=(?P<exp>\d+) .*? dQ_uAh=(?P<dq>-?\d+) dE_uWh=(?P<de>-?\d+) "
    r"Qsum_uAh=(?P<qsum>-?\d+) Esum_uWh=(?P<esum>-?\d+) "
)


def reject(message: str) -> None:
    print(f"[TOTALS] REJECTED: {message}", file=sys.stderr)
    raise SystemExit(1)


def parse(path: pathlib.Path) -> list[dict[str, int]]:
    lines = [x for x in path.read_text(encoding="utf-8").splitlines()
             if x.startswith(RECORD_PREFIX)]

    if not lines:
        reject(f"no decoded record lines in {path}")

    records: list[dict[str, int]] = []
    for line in lines:
        match = RECORD.match(line)
        if match is None:
            reject(f"record line did not parse, refusing to guess:\n{line}")
        else:
            records.append({k: int(v) for k, v in match.groupdict().items()})

    return records


def boundary_from_compare(records: list[dict[str, int]],
                          earlier: pathlib.Path) -> int:
    """Index of the first record of the first boot that began after the preflight.

    The preflight dump was captured minutes before the upload, so the upload's
    reset is the first boot change among records the preflight had not yet seen.
    """

    previous_records = parse(earlier)
    last_known_seq = max(r["seq"] for r in previous_records)

    for i in range(1, len(records)):
        if records[i]["seq"] > last_known_seq and \
                records[i]["boot"] != records[i - 1]["boot"]:
            return i

    reject(f"no boot change after seq {last_known_seq} (the last sequence in "
           f"{earlier.name}), so this dump contains no reset to check. Either "
           f"the board never restarted after the upload, or the wrong dumps "
           f"were paired.")
    raise AssertionError("unreachable")


def boundary_inferred(records: list[dict[str, int]]) -> int:
    newest_boot = max(r["boot"] for r in records)
    position = next(i for i, r in enumerate(records) if r["boot"] == newest_boot)

    if position == 0:
        reject("the newest boot is the first record in the log, so there is no "
               "preceding record it could have continued from.")

    return position


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=pathlib.Path)
    parser.add_argument("--compare", type=pathlib.Path, default=None,
                        help="the preflight dump, which identifies the upload's "
                             "reset. Strongly preferred over inference.")
    options = parser.parse_args()

    path: pathlib.Path = options.dump
    records = parse(path)

    if options.compare is not None:
        position = boundary_from_compare(records, options.compare)
        how = f"identified from {options.compare.name}"
    else:
        position = boundary_inferred(records)
        how = ("INFERRED as the newest boot. This is the upload's boot only if "
               "the board has not restarted since. Pass --compare to be sure.")

    previous = records[position - 1]
    current = records[position]

    expected_qsum = previous["qsum"] + current["dq"]
    expected_esum = previous["esum"] + current["de"]

    # D-062: the sequence decision is not visible in any command's output - INFO
    # does not run it, and the recovery transcript happens at a cold boot the
    # sender does not capture. Its CONSEQUENCE is visible right here. On a
    # healthy log, D-023 makes the log the authority and the next sequence is
    # lastSeq + 1, so the boundary is contiguous. A jump would mean the NVS
    # reservation floor was applied instead, which on a log the board itself
    # reported INTACT would be a regression in the decision.
    sequence_contiguous = current["seq"] == previous["seq"] + 1

    breaks = [
        {"from_seq": a["seq"], "to_seq": b["seq"],
         "from_boot": a["boot"], "to_boot": b["boot"]}
        for a, b in zip(records, records[1:])
        if b["qsum"] != a["qsum"] + b["dq"] or b["esum"] != a["esum"] + b["de"]
    ]

    result = {
        "file": path.name,
        "records": len(records),
        "boundary_how": how,
        "boundary": {
            "last_before_reset": {
                "seq": previous["seq"], "boot": previous["boot"],
                "qsum_uAh": previous["qsum"], "esum_uWh": previous["esum"],
            },
            "first_after_reset": {
                "seq": current["seq"], "boot": current["boot"],
                "dq_uAh": current["dq"], "de_uWh": current["de"],
                "qsum_uAh": current["qsum"], "esum_uWh": current["esum"],
            },
            "expected_qsum_uAh": expected_qsum,
            "expected_esum_uWh": expected_esum,
            "expected_seq": previous["seq"] + 1,
            "sequence_contiguous": sequence_contiguous,
        },
        "other_continuity_breaks_reported_not_asserted": breaks,
    }

    print(json.dumps(result, indent=2))

    if not sequence_contiguous:
        reject("the sequence did NOT continue across the reset. Expected "
               f"seq {previous['seq'] + 1}, got {current['seq']}. On a healthy "
               "log D-023 makes the log the authority, so recovery should have "
               "chosen lastSeq + 1; a jump means the NVS reservation floor was "
               "applied to a log the board reported INTACT (D-062).")

    if current["qsum"] != expected_qsum or current["esum"] != expected_esum:
        reject("the running totals did NOT continue across the reset. Expected "
               f"Qsum_uAh={expected_qsum} Esum_uWh={expected_esum}, got "
               f"Qsum_uAh={current['qsum']} Esum_uWh={current['esum']}. The "
               "reseed in autoStorageRecover() did not run on a healthy log.")

    if options.compare is None:
        print("[TOTALS] WARNING: the boundary was inferred, not identified. "
              "Pass --compare <preflight-dump> so this checks the upload's own "
              "reset.", file=sys.stderr)

    print(f"[TOTALS] D-060 healthy-path reseed: ALL OK (seq {previous['seq']} -> "
          f"{current['seq']} across boot {previous['boot']} -> "
          f"{current['boot']})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
