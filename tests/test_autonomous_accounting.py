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
                    real artifact rather than on a description of it. They read
                    the whole sketch as tokens through tests/firmware_source.py,
                    so they survive the modularization moving these functions
                    into other files, and they are part of the
                    pre-modularization gate (docs/DECISIONS.md D-043).

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

import pytest

import firmware_source


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
def sketch():
    return firmware_source.load()


# (retained RTC total, provisional local, interval delta local,
#  record running-total field, record interval field)
RTC_TOTALS = (
    pytest.param(
        "rtcAutoRunChargeUAh",
        "provisionalRunChargeUAh",
        "intervalChargeUAh",
        "running_charge_uAh",
        "interval_charge_uAh",
        id="charge",
    ),
    pytest.param(
        "rtcAutoRunEnergyUWh",
        "provisionalRunEnergyUWh",
        "intervalEnergyUWh",
        "running_energy_uWh",
        "interval_energy_uWh",
        id="energy",
    ),
)

RETAINED_TOTALS = ("rtcAutoRunChargeUAh", "rtcAutoRunEnergyUWh")


@pytest.mark.parametrize("name", RETAINED_TOTALS)
def test_source_retained_totals_are_never_compound_assigned(sketch, name):
    """`x += delta` is the exact shape of the defect.

    The fix computes a provisional local and assigns it once, after the append
    succeeds. A compound assignment anywhere means someone has advanced the
    retained total in place again, which is how the double-count returns.
    """

    assert not sketch.code.contains(f"{name} +="), (
        f"{name} is compound-assigned in the firmware. The retained total must "
        "be committed once, after a successful durable append."
    )


@pytest.mark.parametrize(
    ("name", "provisional", "delta", "record_total", "record_delta"), RTC_TOTALS
)
def test_source_totals_are_committed_only_after_a_successful_append(
    sketch, name, provisional, delta, record_total, record_delta
):
    """The characterization barrier for autonomous accounting.

    In the wake cycle:

      provisional = retained total + this interval's delta
      the record carries that provisional total and that same delta
      the retained total is assigned EXACTLY ONCE, from that same provisional,
        inside an `if (appended)` block - so a failed append leaves it alone

    Across the sketch, the only other writers are the reseed from the log
    (autoStorageRecover) and the zeroing on LOGGER STORAGE CLEAR YES.
    """

    wake = sketch.function("runAutonomousWakeCycle")

    assert wake.contains(f"const int64_t {provisional} = {name} + {delta};")
    assert wake.contains(f"record.{record_total} = {provisional};")
    assert wake.contains(f"record.{record_delta} = {delta};")

    commits = wake.find_all(f"{name} =")
    assert len(commits) == 1, (
        f"{name} must advance exactly once per wake; assigned {len(commits)} times"
    )
    assert wake.contains(f"{name} = {provisional};"), (
        "the retained total must be the same value the stored record carries"
    )

    appended_blocks = wake.blocks_after("if (appended)")
    assert any(first < commits[0] < last for first, last in appended_blocks), (
        f"{name} is assigned outside the append-success guard, so a failed "
        "append would retain a total the log does not contain."
    )

    assert sketch.functions_containing(f"{name} =") == {
        "runAutonomousWakeCycle",
        "autoStorageRecover",
        "clearStorage",
    }


def test_source_snapshot_failure_writes_sentinels_not_stack_contents(sketch):
    """The second wake-cycle defect: an uninitialized SensorReading.

    readSensor() returns false without writing any field, so the struct must be
    initialized and the record must carry explicit impossible values rather
    than whatever was on the stack.
    """

    wake = sketch.function("runAutonomousWakeCycle")

    assert wake.contains("SensorReading reading = {};"), (
        "the wake cycle's SensorReading must be zero-initialized"
    )

    for sentinel in (
        "record.bus_uV = AUTO_SNAPSHOT_BUS_UV_UNREAD;",
        "record.avg_current_uA = AUTO_SNAPSHOT_SIGNED_UNREAD;",
        "record.avg_power_uW = AUTO_SNAPSHOT_SIGNED_UNREAD;",
        "record.temp_mC = AUTO_SNAPSHOT_SIGNED_UNREAD;",
    ):
        assert wake.contains(sentinel)


def test_source_snapshot_sentinels_are_physically_impossible(sketch):
    # UINT32_MAX microvolts is 4294.97 V against an 85 V part; INT32_MIN
    # microamps is -2147 A. Neither can be produced by a real reading, which is
    # what stops a host mistaking one for a measurement.
    assert sketch.code.contains(
        "constexpr uint32_t AUTO_SNAPSHOT_BUS_UV_UNREAD = UINT32_MAX;"
    )
    assert sketch.code.contains(
        "constexpr int32_t AUTO_SNAPSHOT_SIGNED_UNREAD = INT32_MIN;"
    )


def test_source_record_is_still_seventy_two_bytes(sketch):
    """The on-disk format is frozen; Experiment 3's log depends on it.

    This is the compile-time guard, and it stays. The field-by-field layout is
    pinned separately in test_characterization_record.py.
    """

    assert sketch.code.contains("static_assert(sizeof(AutoRecord) == 72,")


def test_source_experiment_id_is_never_defaulted_on_an_nvs_failure(sketch):
    """D-024: unknown attribution is marked, never replaced with a plausible 0."""

    helper = sketch.function("loadExperimentIdOnly")
    failure_branch = helper.block_after(
        "if (!preferences.begin(NVS_NAMESPACE, true))"
    )

    assert failure_branch.contains("return false;"), (
        "a failed NVS open must report the id as unknown, not return 0 as if it "
        "were a real answer"
    )
    assert not failure_branch.contains("outExperimentId = 0;")
