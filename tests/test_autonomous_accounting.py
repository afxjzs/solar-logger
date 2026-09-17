"""Regression cover for the autonomous running-total double-count.

WHAT THIS DOES AND DOES NOT TEST
================================

The accumulation rule lives inside `runAutonomousWakeCycle()` in the `.ino`,
which is one Arduino translation unit and cannot be compiled on the host
without the modularization that is deliberately NOT part of this stint. So
these tests come in two halves, and the difference between them matters:

  `test_rule_*`     an executable SPECIFICATION of the rule, run against a
                    Python model. It pins what the firmware is supposed to do.
                    It does NOT execute firmware code and proves nothing about
                    the compiled image on its own.

  `test_source_*`   assertions against the actual firmware source, so the
                    property that makes the defect impossible is checked on the
                    real artifact rather than on a description of it.

Together they say: here is the rule, and here is evidence the shipped source
still has the shape that rule requires. Full behavioral cover needs the
firmware modules to exist; until then the hardware acceptance run in
LAB_NOTES.md is what exercises the real path.

THE DEFECT
==========

`rtcAutoRunChargeUAh` was advanced by the interval's charge BEFORE the durable
append. When the append failed, the INA228 accumulators were deliberately left
unreset so the next interval would cover both periods - but the retained totals
had already absorbed the first period. The next wake read both periods out of
the CHARGE register and added the whole thing on top, so the first interval was
counted twice in every later record, permanently: `autoStorageRecover()` reseeds
the totals from the last stored record, so a reboot preserved the error.
"""

from __future__ import annotations

import re

import pytest

from conftest import FIRMWARE


# ============================================================================
# The rule, as an executable specification
# ============================================================================


class WakeAccounting:
    """Models the accumulate/store/reset ordering of one autonomous wake.

    `accumulator` stands in for the INA228 hardware CHARGE register: it keeps
    integrating until something resets it, which is the whole reason the
    ordering matters.
    """

    def __init__(self):
        self.running_total = 0
        self.accumulator = 0
        self.records = []

    def interval_elapses(self, charge):
        """Hardware accumulates, whether or not the last wake stored anything."""

        self.accumulator += charge

    def wake(self, append_succeeds=True):
        """One wake cycle: read, build, append, then commit and reset."""

        interval_charge = self.accumulator

        # Provisional. The record needs a running total, but nothing is retained
        # until the durable write has actually happened.
        provisional = self.running_total + interval_charge

        if not append_succeeds:
            # Accumulators are NOT reset, so the next interval covers both
            # periods - and the retained total must NOT move, or that next
            # interval will be added on top of a total that already contains it.
            return None

        self.records.append(
            {"interval_charge": interval_charge, "running_total": provisional}
        )
        self.running_total = provisional
        self.accumulator = 0
        return self.records[-1]


def test_rule_running_total_is_the_sum_of_stored_intervals():
    acct = WakeAccounting()

    for charge in (116, 120, 118):
        acct.interval_elapses(charge)
        acct.wake()

    assert [r["interval_charge"] for r in acct.records] == [116, 120, 118]
    assert [r["running_total"] for r in acct.records] == [116, 236, 354]


def test_rule_a_failed_append_does_not_double_count():
    """The regression.

    Against the pre-fix rule this sequence produces a running total of 352
    where 236 is correct: the first interval's 116 counted twice. Verified by
    running the pre-fix rule directly before the fix was applied.
    """

    acct = WakeAccounting()

    acct.interval_elapses(116)
    assert acct.wake(append_succeeds=False) is None

    # Nothing was stored and nothing was retained.
    assert acct.records == []
    assert acct.running_total == 0

    # The hardware kept integrating across the failure, so the next wake reads
    # both periods as one interval. That is intended: the charge is covered
    # exactly once rather than lost.
    acct.interval_elapses(120)
    record = acct.wake()

    assert record is not None, "this append succeeds, so a record is produced"
    assert record["interval_charge"] == 236, "both periods, counted once"
    assert record["running_total"] == 236
    assert acct.running_total == 236, (
        "116 must appear exactly once; 352 would be the double-count defect"
    )


def test_rule_repeated_failures_still_count_each_period_once():
    acct = WakeAccounting()

    for charge in (100, 50, 25):
        acct.interval_elapses(charge)
        acct.wake(append_succeeds=False)

    acct.interval_elapses(5)
    record = acct.wake()

    assert record is not None
    assert record["interval_charge"] == 180
    assert acct.running_total == 180


def test_rule_energy_follows_the_same_ordering_as_charge():
    # Same rule, second quantity. They are committed together precisely so they
    # cannot disagree about which intervals they contain.
    acct = WakeAccounting()

    acct.interval_elapses(1579)
    acct.wake(append_succeeds=False)
    acct.interval_elapses(1584)
    record = acct.wake()

    assert record is not None
    assert record["running_total"] == 3163


# ============================================================================
# The shipped source still has the shape the rule requires
# ============================================================================


@pytest.fixture(scope="module")
def firmware_source():
    assert FIRMWARE.exists(), f"firmware source not found at {FIRMWARE}"
    return FIRMWARE.read_text(encoding="utf-8")


RTC_TOTALS = ("rtcAutoRunChargeUAh", "rtcAutoRunEnergyUWh")


@pytest.mark.parametrize("name", RTC_TOTALS)
def test_source_retained_totals_are_never_compound_assigned(firmware_source, name):
    """`x += delta` is the exact shape of the defect.

    The fix computes a provisional local and assigns it once, after the append
    succeeds. A compound assignment anywhere means someone has advanced the
    retained total in place again, which is how the double-count returns.
    """

    offenders = re.findall(rf"{name}\s*\+=", firmware_source)

    assert offenders == [], (
        f"{name} is compound-assigned in the firmware. The retained total must "
        "be committed once, after a successful durable append."
    )


@pytest.mark.parametrize("name", RTC_TOTALS)
def test_source_totals_are_committed_only_after_a_successful_append(
    firmware_source, name
):
    """Inside the wake cycle, the commit must sit in the `if (appended)` block."""

    start = firmware_source.index("bool runAutonomousWakeCycle()")
    end = firmware_source.index("void autonomousDeepSleepAgain(", start)
    wake_cycle = firmware_source[start:end]

    assignments = [
        match.start()
        for match in re.finditer(rf"^\t*{name}\s*=", wake_cycle, re.MULTILINE)
    ]

    assert assignments, f"{name} is never committed in the wake cycle"

    guard = wake_cycle.index("if (appended)\n\t{\n\t\t// Committed here")

    for position in assignments:
        assert position > guard, (
            f"{name} is assigned before the append-success guard, so a failed "
            "append would retain a total the log does not contain."
        )


def test_source_snapshot_failure_writes_sentinels_not_stack_contents(firmware_source):
    """The second wake-cycle defect: an uninitialized SensorReading.

    readSensor() returns false without writing any field, so the struct must be
    initialized and the record must carry explicit impossible values rather
    than whatever was on the stack.
    """

    assert "SensorReading reading = {};" in firmware_source, (
        "the wake cycle's SensorReading must be zero-initialized"
    )

    for sentinel in (
        "record.bus_uV = AUTO_SNAPSHOT_BUS_UV_UNREAD;",
        "record.avg_current_uA = AUTO_SNAPSHOT_SIGNED_UNREAD;",
        "record.avg_power_uW = AUTO_SNAPSHOT_SIGNED_UNREAD;",
        "record.temp_mC = AUTO_SNAPSHOT_SIGNED_UNREAD;",
    ):
        assert sentinel in firmware_source


def test_source_snapshot_sentinels_are_physically_impossible(firmware_source):
    # UINT32_MAX microvolts is 4294.97 V against an 85 V part; INT32_MIN
    # microamps is -2147 A. Neither can be produced by a real reading, which is
    # what stops a host mistaking one for a measurement.
    assert (
        "constexpr uint32_t AUTO_SNAPSHOT_BUS_UV_UNREAD = UINT32_MAX;"
        in firmware_source
    )
    assert (
        "constexpr int32_t AUTO_SNAPSHOT_SIGNED_UNREAD = INT32_MIN;"
        in firmware_source
    )


def test_source_record_is_still_seventy_two_bytes(firmware_source):
    """The on-disk format is frozen; Experiment 3's log depends on it."""

    assert "static_assert(sizeof(AutoRecord) == 72," in firmware_source


def test_source_experiment_id_is_never_defaulted_on_an_nvs_failure(firmware_source):
    """D-024: unknown attribution is marked, never replaced with a plausible 0."""

    start = firmware_source.index("bool loadExperimentIdOnly(")
    end = firmware_source.index("bool saveAutonomousTestArmed(", start)
    helper = firmware_source[start:end]

    open_failure = helper.index("if (!preferences.begin(NVS_NAMESPACE, true))")
    next_branch = helper.index("if (!preferences.isKey(\"schema\"))", open_failure)
    failure_branch = helper[open_failure:next_branch]

    assert "return false;" in failure_branch, (
        "a failed NVS open must report the id as unknown, not return 0 as if it "
        "were a real answer"
    )
    assert "outExperimentId = 0;" not in failure_branch
