"""Execute the offline report on synthetic captures; never touch the board."""
from __future__ import annotations

import importlib.util
from collections.abc import Sequence
from datetime import datetime, timedelta
from pathlib import Path
import subprocess
import sys

import pandas as pd
import pytest

SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "charger-transitions.py"
# The script keeps its existing command-line filename; load it without renaming it.
spec = importlib.util.spec_from_file_location("charger_transitions", SCRIPT)
assert spec and spec.loader
charger = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = charger
spec.loader.exec_module(charger)


def capture(currents: Sequence[float], seconds: Sequence[float] | None = None) -> pd.DataFrame:
    origin = datetime.fromisoformat("2026-09-24T14:00:00-07:00")
    offsets = seconds if seconds is not None else list(range(len(currents)))
    return pd.DataFrame({
        "captured_at": [(origin + timedelta(seconds=value)).isoformat() for value in offsets],
        "experiment_id": [3] * len(currents),
        "voltage_V": [13.0] * len(currents),
        "current_mA": currents,
        "power_mW": [abs(value) * 13 for value in currents],
    })


def test_initial_state_and_both_contiguous_edges_keep_start_and_confirmation(capsys):
    frame = capture([-0.6] * 5 + [100] * 5 + [-0.6] * 5)
    frame.index = [100 + 7 * value for value in range(len(frame))]
    report = charger.analyze(frame)
    assert [(o.kind, o.state, o.start, o.confirmed) for o in report.observations] == [
        ("INITIAL STATE", "NOT_CHARGING", 0, 4),
        (charger.OBSERVED, "CHARGING", 5, 9),
        (charger.OBSERVED, "NOT_CHARGING", 10, 14),
    ]
    charger.print_report(report)
    output = capsys.readouterr().out
    assert "Sample bracket: 2026-09-24 14:00:04-07:00 -> 2026-09-24 14:00:05-07:00" in output
    assert "Confirmed at: 2026-09-24 14:00:09-07:00" in output
    assert "Observed contiguous transitions: 2" in output
    assert "Last captured row:  2026-09-24 14:00:14-07:00" in output
    assert "Exact physical switching instant UNKNOWN" in output


@pytest.mark.parametrize("after", [-0.6, 100.0])
def test_gap_reacquires_state_even_when_unchanged_and_clips_context(after, capsys):
    report = charger.analyze(capture([-0.6] * 5 + [after] * 5, [0, 1, 2, 3, 4, 100, 101, 102, 103, 104]))
    assert [o.kind for o in report.observations] == ["INITIAL STATE", "STATE AFTER CAPTURE GAP"]
    assert report.segments[1].separation_seconds == 96
    charger.print_report(report)
    post_gap = capsys.readouterr().out.split("SEGMENT 2:")[1]
    context = post_gap.split("Surrounding samples (clipped to this capture segment):")[1]
    assert "14:00:04" not in context
    assert "14:01:40" in context
    assert "Transition timing UNKNOWN" in post_gap


def test_persistence_does_not_bridge_a_gap_and_short_segments_are_visible(capsys):
    report = charger.analyze(capture([100] * 6, [0, 1, 2, 100, 101, 102]))
    assert report.observations == []
    charger.print_report(report)
    output = capsys.readouterr().out
    assert output.count("No confirmed state") == 2
    assert "Capture gaps: 1" in output


@pytest.mark.parametrize("same_state", [True, False])
def test_experiment_boundary_resets_confirmed_state_and_persistence(same_state):
    frame = capture([-0.6] * 5 + ([-0.6] if same_state else [100]) * 5)
    frame.loc[5:, "experiment_id"] = 4
    report = charger.analyze(frame)
    assert [o.kind for o in report.observations] == ["INITIAL STATE", "STATE AFTER EXPERIMENT CHANGE"]
    partial = capture([100] * 6)
    partial.loc[3:, "experiment_id"] = 4
    assert charger.analyze(partial).observations == []


@pytest.mark.parametrize("offsets", [[0, 1, 2, 3, 4, 4, 5, 6, 7, 8], [0, 1, 2, 3, 4, 2, 3, 4, 5, 6]])
def test_duplicate_or_reversed_time_is_reported_without_sorting(offsets, capsys):
    report = charger.analyze(capture([-0.6] * 5 + [100] * 5, offsets))
    assert report.observations[1].kind == "STATE AFTER NON-INCREASING CAPTURE TIME"
    charger.print_report(report)
    assert "NON-INCREASING CAPTURE TIME" in capsys.readouterr().out


@pytest.mark.parametrize("ambiguous", [10.0, float("nan"), float("inf")])
def test_ambiguous_samples_break_persistence_and_prevent_a_false_timed_edge(ambiguous):
    report = charger.analyze(capture([-0.6] * 5 + [ambiguous] + [100] * 5))
    assert [o.kind for o in report.observations] == ["INITIAL STATE", "STATE CHANGE WITH UNCERTAIN EDGE"]
    report = charger.analyze(capture([100] * 3 + [ambiguous] + [100] * 3))
    assert report.observations == []


def test_short_excursion_is_not_a_sustained_transition():
    report = charger.analyze(capture([-0.6] * 5 + [100] * 4 + [-0.6] * 5))
    assert [o.kind for o in report.observations] == ["INITIAL STATE"]


def test_previous_side_requires_fresh_persistence_before_an_observed_edge():
    report = charger.analyze(capture([-0.6] * 5 + [10] + [-0.6] * 2 + [100] * 5))
    assert report.observations[-1].kind == "STATE CHANGE WITH UNCERTAIN EDGE"
    report = charger.analyze(capture([-0.6] * 5 + [10] + [-0.6] * 5 + [100] * 5))
    assert report.observations[-1].kind == charger.OBSERVED


def test_gap_limit_is_inclusive_and_configurable():
    frame = capture([-0.6] * 5 + [100] * 5, [0, 1, 2, 3, 4, 6, 7, 8, 9, 10])
    assert charger.analyze(frame).observations[-1].kind == charger.OBSERVED
    assert charger.analyze(frame, max_gap_seconds=1.5).observations[-1].kind == "STATE AFTER CAPTURE GAP"
    assert charger.analyze(capture([100] * 3), persistence_samples=3).observations[0].confirmed == 2


def test_empty_capture_and_insufficient_samples_are_explicit(capsys):
    charger.print_report(charger.analyze(capture([])))
    assert "No samples; no state or transition evidence" in capsys.readouterr().out
    report = charger.analyze(capture([100] * 4))
    assert report.observations == []
    charger.print_report(report)
    assert "No confirmed state" in capsys.readouterr().out


@pytest.mark.parametrize("field,value", [("captured_at", "bad time"), ("captured_at", "2026-09-24T14:00:00"), ("experiment_id", float("nan")), ("current_mA", "bad current")])
def test_invalid_input_fails_explicitly(field, value):
    frame = capture([100])
    frame[field] = [value]
    with pytest.raises(ValueError):
        charger.analyze(frame)


def test_missing_schema_fails_explicitly():
    with pytest.raises(ValueError, match="Missing required columns"):
        charger.analyze(pd.DataFrame({"captured_at": []}))


def test_offset_change_uses_absolute_spacing_and_preserves_offsets():
    frame = capture([100] * 5)
    frame["captured_at"] = ["2026-11-01T01:59:58-07:00", "2026-11-01T01:59:59-07:00",
                            "2026-11-01T01:00:00-08:00", "2026-11-01T01:00:01-08:00",
                            "2026-11-01T01:00:02-08:00"]
    report = charger.analyze(frame)
    assert len(report.segments) == 1
    assert str(report.samples.iloc[-1]["captured_at"]).endswith("-08:00")


@pytest.mark.parametrize("option,value", [("--max-gap-seconds", "nan"), ("--max-gap-seconds", "0"), ("--persistence-samples", "0")])
def test_cli_invalid_settings_exit_nonzero(tmp_path, option, value):
    path = tmp_path / "samples.csv"
    capture([100] * 5).to_csv(path, index=False)
    result = subprocess.run([sys.executable, str(SCRIPT), str(path), option, value], capture_output=True, text=True)
    assert result.returncode == 1
    assert "ERROR:" in result.stderr
    assert "CHARGER OUTPUT CAPTURE REPORT" not in result.stdout
