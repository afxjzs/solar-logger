#!/usr/bin/env python3
"""Characterize captured charger output; never infer switching times across gaps."""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
from datetime import datetime
import math
from pathlib import Path

import pandas as pd


CSV_PATH = Path("data/samples.csv")

# Analysis settings only, NOT production firmware thresholds.
VALID_VOLTAGE_MIN_V = 10.0
VALID_VOLTAGE_MAX_V = 16.0
CHARGING_CURRENT_MA = 20.0
NOT_CHARGING_CURRENT_MA = 5.0
PERSISTENCE_SAMPLES = 5
# Nominal CSV cadence is 1 s (measured median 1.000141 s, 2026-09-25).
# Allow up to two periods; print this provisional choice and allow an override.
MAX_GAP_SECONDS = 2.0
MEASUREMENTS = ["voltage_V", "current_mA", "power_mW"]
REQUIRED_COLUMNS = ["captured_at", "experiment_id", *MEASUREMENTS]
OBSERVED = "OBSERVED CONTIGUOUS TRANSITION"


@dataclass
class Segment:
    start: int
    stop: int  # exclusive, positional (never a DataFrame index label)
    reason: str
    separation_seconds: float | None = None


@dataclass
class Observation:
    kind: str
    state: str
    previous_state: str | None
    start: int
    confirmed: int
    segment: int


@dataclass
class Report:
    samples: pd.DataFrame
    segments: list[Segment]
    observations: list[Observation]
    max_gap_seconds: float
    persistence_samples: int


def classify_sample(row) -> str:
    """Keep ambiguous/nonfinite measurements out of persistence evidence."""
    voltage, current, power = (float(row[name]) for name in MEASUREMENTS)
    if not all(math.isfinite(value) for value in (voltage, current, power)):
        return "UNKNOWN"
    if not VALID_VOLTAGE_MIN_V <= voltage <= VALID_VOLTAGE_MAX_V:
        return "UNKNOWN"
    if current >= CHARGING_CURRENT_MA:
        return "CHARGING"
    if current <= NOT_CHARGING_CURRENT_MA:
        return "NOT_CHARGING"
    return "TRANSITION"


def prepare_samples(frame: pd.DataFrame) -> pd.DataFrame:
    missing = set(REQUIRED_COLUMNS) - set(frame.columns)
    if missing:
        raise ValueError(f"Missing required columns: {', '.join(sorted(missing))}")
    df = frame.copy()
    times = []
    for position, value in enumerate(df["captured_at"]):
        try:
            timestamp = datetime.fromisoformat(str(value))
            if timestamp.utcoffset() is None:
                raise ValueError("timezone offset required")
        except ValueError as error:
            raise ValueError(f"CSV row {position + 2}: invalid captured_at {value!r}: {error}") from error
        times.append(timestamp)
    # Preserve original offsets, including files spanning a DST offset change.
    df["captured_at"] = pd.array(times, dtype=object)
    for name in ["experiment_id", *MEASUREMENTS]:
        try:
            df[name] = pd.to_numeric(df[name], errors="raise")
        except (TypeError, ValueError) as error:
            raise ValueError(f"Invalid numeric column {name}: {error}") from error
    for position, value in enumerate(df["experiment_id"]):
        if not math.isfinite(float(value)) or value < 0 or value != int(value):
            raise ValueError(f"CSV row {position + 2}: invalid experiment_id {value!r}")
    df["raw_state"] = [classify_sample(row) for _, row in df.iterrows()]
    return df


def analyze(
    frame: pd.DataFrame,
    *,
    max_gap_seconds: float = MAX_GAP_SECONDS,
    persistence_samples: int = PERSISTENCE_SAMPLES,
) -> Report:
    if not math.isfinite(max_gap_seconds) or max_gap_seconds <= 0:
        raise ValueError("max-gap-seconds must be finite and greater than zero")
    if persistence_samples < 1:
        raise ValueError("persistence-samples must be at least 1")
    df = prepare_samples(frame)
    segments: list[Segment] = []
    for position in range(len(df)):
        reason = "FILE START"
        separation = None
        if position:
            previous, current = df.iloc[position - 1], df.iloc[position]
            separation = (current["captured_at"] - previous["captured_at"]).total_seconds()
            reasons = []
            if current["experiment_id"] != previous["experiment_id"]:
                reasons.append("EXPERIMENT CHANGE")
            if separation <= 0:
                reasons.append("NON-INCREASING CAPTURE TIME")
            elif separation > max_gap_seconds:
                reasons.append("CAPTURE GAP")
            if not reasons:
                continue
            reason = " + ".join(reasons)
            segments[-1].stop = position
        segments.append(Segment(position, len(df), reason, separation))

    observations: list[Observation] = []
    for segment_index, segment in enumerate(segments):
        # Neither confirmed state nor partial persistence survives a boundary.
        confirmed_state = None
        candidate_state = None
        candidate_count = 0
        preceding_state = None
        for position in range(segment.start, segment.stop):
            raw_state = str(df.iloc[position]["raw_state"])
            if raw_state in {"UNKNOWN", "TRANSITION"}:
                candidate_state = None
                candidate_count = 0
                continue
            if raw_state == candidate_state:
                candidate_count += 1
            else:
                preceding_state = candidate_state if candidate_count >= persistence_samples else None
                candidate_state = raw_state
                candidate_count = 1
            if candidate_count != persistence_samples or candidate_state == confirmed_state:
                continue
            if not observations:
                kind = "INITIAL STATE"
            elif confirmed_state is None:
                kind = f"STATE AFTER {segment.reason}"
            elif preceding_state == confirmed_state:
                kind = OBSERVED
            else:
                kind = "STATE CHANGE WITH UNCERTAIN EDGE"
            observations.append(Observation(
                kind, raw_state, confirmed_state,
                position - persistence_samples + 1, position, segment_index,
            ))
            confirmed_state = raw_state
    return Report(df, segments, observations, max_gap_seconds, persistence_samples)


def print_report(report: Report) -> None:
    df = report.samples
    print("CHARGER OUTPUT CAPTURE REPORT")
    print("Analysis settings only; no cause or solar-availability inference.")
    print(f"Maximum adjacent capture spacing: {report.max_gap_seconds:g} s (inclusive)")
    print(f"Persistence: {report.persistence_samples} consecutive classified samples per state")
    print(f"Classification: V={VALID_VOLTAGE_MIN_V:g}..{VALID_VOLTAGE_MAX_V:g} V; "
          f"CHARGING >= {CHARGING_CURRENT_MA:g} mA; "
          f"NOT_CHARGING <= {NOT_CHARGING_CURRENT_MA:g} mA; otherwise ambiguous")
    print(f"Samples: {len(df)}; capture segments: {len(report.segments)}")
    if df.empty:
        print("No samples; no state or transition evidence.")
        return
    print(f"First captured row: {df.iloc[0]['captured_at']}")
    print(f"Last captured row:  {df.iloc[-1]['captured_at']}")
    print("Host receipt timestamps, not exact physical switching times; file span is not continuous coverage.")
    counts = Counter(df["raw_state"])
    print(f"Ambiguous samples: UNKNOWN={counts['UNKNOWN']}, TRANSITION={counts['TRANSITION']}")
    for segment_index, segment in enumerate(report.segments):
        first, last = df.iloc[segment.start], df.iloc[segment.stop - 1]
        print(f"\nSEGMENT {segment_index + 1}: {segment.reason}; experiment {int(first['experiment_id'])}")
        print(f"  Capture: {first['captured_at']} .. {last['captured_at']} ({segment.stop - segment.start} samples)")
        if segment.start:
            previous = df.iloc[segment.start - 1]
            print(f"  Boundary: {previous['captured_at']} (exp {int(previous['experiment_id'])}) -> "
                  f"{first['captured_at']} (exp {int(first['experiment_id'])}); "
                  f"separation {segment.separation_seconds:.6f} s")
            print("  Persistence restarted; intervening states and transition timing UNKNOWN.")
        found = [item for item in report.observations if item.segment == segment_index]
        if not found:
            print("  No confirmed state: insufficient consecutive classified samples.")
        for observation in found:
            start = df.iloc[observation.start]
            print(f"\n  {observation.kind}: {observation.state}")
            print(f"  First supporting sample: {start['captured_at']}")
            print(f"  Confirmed at: {df.iloc[observation.confirmed]['captured_at']}")
            if observation.kind == OBSERVED:
                before = df.iloc[observation.start - 1]
                print(f"  Sample bracket: {before['captured_at']} -> {start['captured_at']} "
                      f"({observation.previous_state} -> {observation.state})")
                print("  Exact physical switching instant UNKNOWN.")
            else:
                print("  Transition timing UNKNOWN; this is a state observation, not a timed edge.")
            print(f"  V={start['voltage_V']:.6f} V  I={start['current_mA']:.3f} mA  P={start['power_mW']:.3f} mW")
            before_position = max(segment.start, observation.start - 3)
            after_position = min(segment.stop, max(observation.start + 8, observation.confirmed + 1))
            print("  Surrounding samples (clipped to this capture segment):")
            print(df.iloc[before_position:after_position][[*REQUIRED_COLUMNS, "raw_state"]].to_string(index=False))
    kinds = Counter(item.kind for item in report.observations)
    print(f"\nObserved contiguous transitions: {kinds[OBSERVED]}")
    print(f"State observations without a timed edge: {len(report.observations) - kinds[OBSERVED]}")
    print(f"Capture gaps: {sum('CAPTURE GAP' in segment.reason for segment in report.segments)}")
    print("Same state on both sides of a gap does not prove it persisted inside the gap.")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv_path", nargs="?", type=Path, default=CSV_PATH)
    parser.add_argument("--max-gap-seconds", type=float, default=MAX_GAP_SECONDS,
                        help="maximum adjacent spacing within a capture segment (default: %(default)s)")
    parser.add_argument("--persistence-samples", type=int, default=PERSISTENCE_SAMPLES,
                        help="consecutive classified samples to confirm each state (default: %(default)s)")
    args = parser.parse_args()
    try:
        report = analyze(pd.read_csv(args.csv_path), max_gap_seconds=args.max_gap_seconds,
                         persistence_samples=args.persistence_samples)
    except (OSError, ValueError, pd.errors.ParserError, pd.errors.EmptyDataError) as error:
        parser.exit(1, f"ERROR: {error}\n")
    print_report(report)


if __name__ == "__main__":
    main()
