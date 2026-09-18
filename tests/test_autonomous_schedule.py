"""The autonomous deadline scheduler, executed on the host (D-032, D-034, D-046).

WHAT RUNS HERE
==============

Three pure functions in the firmware carry the autonomous timing arithmetic:

  autonomousHandoffElapsedMs()   session time at a host handoff
  autonomousElapsedAtSleepMs()   session time when a path goes to sleep
  planAutonomousSleep()          the deadline decision: how long to sleep

The `scheduler` fixture takes their source out of the sketch through
tests/firmware_source.py, so it still finds them after they move to another
file. It compiles them on the host with the enum and struct they use, and the
tests run them. Every sleep, boundary and session time asserted below is
computed by the firmware's own code.

What is NOT executed is the composition: which clock readings the scheduler
and the handoff pass in, and the RTC-retained values they keep between calls.
`Board` does that composition the way the firmware does, and the test_source_*
tests pin that the firmware still does it that way. How the board actually
behaves is the hardware acceptance sequence in docs/LAB_NOTES.md.

THE DEFECT (fixed 2026-09-18, D-046)
====================================

After a RELEASE or a lease expiry, the handoff set rtcAutoSessionElapsedMs to
the session time of the handoff, which already includes the time the board had
been awake. The scheduler then added the time since setup() entry to it again.
A board released 28 s after its wake slept 31,750 ms instead of 59,750 ms, and
the next record still reported an interval of about 60 s.
"""

from __future__ import annotations

import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

import pytest

import firmware_source

NOMINAL_MS = 60_000  # LOGGER INTERVAL 60, the firmware's default cadence
GRACE_MS = 250  # SESSION_RELEASE_ACK_GRACE_MS, pinned by a source test below
BOOTLOADER_MS = 300  # millis() at setup() entry; any value, it only offsets
READ_MS = 150  # a wake cycle's accumulator read, after the cycle began
RESET_MS = 180  # its accumulator reset, after the append

HELPERS = (
    ("autonomousHandoffElapsedMs", "uint32_t"),
    ("autonomousElapsedAtSleepMs", "uint32_t"),
    ("planAutonomousSleep", "AutoSleepPlan"),
)


def u32(value: int) -> int:
    return value % 2**32


# ============================================================================
# The harness: the firmware's own functions, compiled for the host
# ============================================================================

PRELUDE = """\
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
"""

DRIVER = r"""
static uint32_t number(const char *text)
{
	char *end = nullptr;
	errno = 0;
	const unsigned long long value = std::strtoull(text, &end, 10);

	if (text[0] < '0' || text[0] > '9' || errno != 0 || *end != '\0' ||
			value > UINT32_MAX)
	{
		std::fprintf(stderr, "not a uint32_t: %s\n", text);
		std::exit(2);
	}

	return static_cast<uint32_t>(value);
}

static bool flag(const char *text)
{
	if (std::strcmp(text, "true") == 0)
	{
		return true;
	}

	if (std::strcmp(text, "false") == 0)
	{
		return false;
	}

	std::fprintf(stderr, "not a bool: %s\n", text);
	std::exit(2);
}

static AutoSleepPath path_named(const char *name)
{
@PATHS@
	std::fprintf(stderr, "unknown AutoSleepPath: %s\n", name);
	std::exit(2);
}

int main(int argc, char **argv)
{
	if (argc == 5 && std::strcmp(argv[1], "plan") == 0)
	{
		const AutoSleepPlan plan =
				planAutonomousSleep(number(argv[2]), number(argv[3]), number(argv[4]));
		std::printf("%u %u %u %u %d\n", static_cast<unsigned>(plan.sleepMs),
								static_cast<unsigned>(plan.intervalStartMs),
								static_cast<unsigned>(plan.wakeElapsedMs),
								static_cast<unsigned>(plan.overrunMs), plan.overran ? 1 : 0);
		return 0;
	}

	if (argc == 7 && std::strcmp(argv[1], "elapsed-at-sleep") == 0)
	{
		std::printf("%u\n", static_cast<unsigned>(autonomousElapsedAtSleepMs(
														path_named(argv[2]), number(argv[3]), number(argv[4]),
														number(argv[5]), number(argv[6]))));
		return 0;
	}

	if (argc == 6 && std::strcmp(argv[1], "handoff-elapsed") == 0)
	{
		std::printf("%u\n", static_cast<unsigned>(autonomousHandoffElapsedMs(
														flag(argv[2]), number(argv[3]), number(argv[4]),
														number(argv[5]))));
		return 0;
	}

	std::fprintf(stderr, "unknown or malformed command\n");
	return 2;
}
"""


def sleep_paths(sketch: firmware_source.Sketch) -> list[str]:
    """The AutoSleepPath enumerators, in source order."""

    enum = sketch.type_definition("enum", "AutoSleepPath").texts
    body = enum[enum.index("{") + 1 : enum.index("}")]
    return [text for text in body if text.startswith("AUTO_SLEEP_")]


def harness_source(sketch: firmware_source.Sketch) -> str:
    extracted = [
        str(sketch.type_definition("enum", "AutoSleepPath")),
        str(sketch.type_definition("struct", "AutoSleepPlan")),
        *(str(sketch.definition(name, returns)) for name, returns in HELPERS),
    ]
    paths = "\n".join(
        f'\tif (std::strcmp(name, "{path}") == 0)\n\t{{\n\t\treturn {path};\n\t}}\n'
        for path in sleep_paths(sketch)
    )
    return "\n\n".join([PRELUDE, *extracted, DRIVER.replace("@PATHS@", paths)])


@dataclass(frozen=True)
class Plan:
    sleep_ms: int
    interval_start_ms: int
    wake_elapsed_ms: int
    overrun_ms: int
    overran: bool


class Scheduler:
    """Calls into the compiled firmware functions, one process per call."""

    def __init__(self, binary: Path):
        self.binary = binary

    def _run(self, *arguments: object) -> str:
        result = subprocess.run(
            [str(self.binary), *(str(argument) for argument in arguments)],
            capture_output=True,
            text=True,
            check=False,
        )

        if result.returncode != 0:
            raise RuntimeError(
                f"harness {arguments} exited {result.returncode}: {result.stderr}"
            )

        return result.stdout.strip()

    def plan(self, interval_start_ms: int, now_elapsed_ms: int, nominal_ms: int) -> Plan:
        fields = self._run("plan", interval_start_ms, now_elapsed_ms, nominal_ms).split()
        sleep, start, wake, overrun, overran = (int(field) for field in fields)
        return Plan(sleep, start, wake, overrun, overran == 1)

    def elapsed_at_sleep(
        self,
        path: str,
        retained_elapsed_ms: int,
        awake_ms: int,
        since_handoff_ms: int,
        boot_millis_ms: int,
    ) -> int:
        return int(
            self._run(
                "elapsed-at-sleep",
                path,
                retained_elapsed_ms,
                awake_ms,
                since_handoff_ms,
                boot_millis_ms,
            )
        )

    def handoff_elapsed(
        self,
        continues_retained_clock: bool,
        retained_elapsed_ms: int,
        since_setup_entry_ms: int,
        boot_millis_ms: int,
    ) -> int:
        return int(
            self._run(
                "handoff-elapsed",
                "true" if continues_retained_clock else "false",
                retained_elapsed_ms,
                since_setup_entry_ms,
                boot_millis_ms,
            )
        )


@pytest.fixture(scope="module")
def scheduler(tmp_path_factory: pytest.TempPathFactory) -> Scheduler:
    compiler = shutil.which("c++")

    if compiler is None:
        pytest.fail(
            "No host C++ compiler (`c++`) on PATH. These tests compile the "
            "firmware's timing functions and run them; without a compiler the "
            "scheduler has no executed cover, so this is a failure, not a skip."
        )

    source = harness_source(firmware_source.load())
    build = tmp_path_factory.mktemp("schedule")
    cpp = build / "schedule_harness.cpp"
    binary = build / "schedule_harness"
    cpp.write_text(source, encoding="utf-8")

    # -std=c++20 because the ESP32 core builds with -std=gnu++2a. -Werror, so
    # -Wswitch fails the run when a new AutoSleepPath has no clock chosen.
    result = subprocess.run(
        [compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-o", str(binary), str(cpp)],
        capture_output=True,
        text=True,
        check=False,
    )

    if result.returncode != 0:
        pytest.fail(
            "The firmware's timing functions did not compile on the host:\n"
            f"{result.stderr}\n--- harness source ---\n{source}"
        )

    return Scheduler(binary)


# ============================================================================
# The composition, as the firmware does it
# ============================================================================


@dataclass
class Board:
    """The retained timing state, driven through the compiled functions.

    Each method is one firmware step with the same clock readings the firmware
    passes. `since_setup_ms` is time since setup() entry on the current boot.
    runAutonomousWakeCycle() measures from its own entry, `cycle_entry_ms`
    after setup() entry; the scheduler measures from setup() entry.
    """

    scheduler: Scheduler
    session_ms: int  # rtcAutoSessionElapsedMs
    interval_start_ms: int  # rtcAutoIntervalStartMs
    nominal_ms: int = NOMINAL_MS
    handoff_at_ms: int = 0  # hostHandoffAtMs; boot-local RAM, 0 after any boot
    cycle_entry_ms: int = 0

    def millis(self, since_setup_ms: int) -> int:
        return u32(BOOTLOADER_MS + since_setup_ms)

    @staticmethod
    def awake_ms(since_setup_ms: int) -> int:
        """`(micros() - setupEntryMicros) / 1000UL`: micros() wraps at 2**32 us."""

        return u32(since_setup_ms * 1000) // 1000

    def new_boot(self) -> None:
        self.handoff_at_ms = 0

    # runAutonomousWakeCycle() ----------------------------------------------

    def wake_record(self, since_setup_ms: int) -> dict[str, int]:
        """The record's two timing fields, as the wake cycle computes them."""

        now = u32(self.session_ms + since_setup_ms - self.cycle_entry_ms)
        return {
            "session_elapsed_ms": now,
            "interval_ms": u32(now - self.interval_start_ms),
        }

    def wake_reset(self, since_setup_ms: int) -> None:
        """The accumulator reset after a stored record: the new boundary."""

        self.interval_start_ms = u32(self.session_ms + since_setup_ms - self.cycle_entry_ms)

    # autonomousDeepSleepAgain() --------------------------------------------

    def sleep(self, path: str, since_setup_ms: int) -> Plan:
        now_ms = self.millis(since_setup_ms)
        now = self.scheduler.elapsed_at_sleep(
            path,
            self.session_ms,
            self.awake_ms(since_setup_ms),
            u32(now_ms - self.handoff_at_ms),
            now_ms,
        )
        plan = self.scheduler.plan(self.interval_start_ms, now, self.nominal_ms)
        self.interval_start_ms = plan.interval_start_ms
        self.session_ms = plan.wake_elapsed_ms
        self.new_boot()
        return plan

    # beginAutonomousSleepFromHostSession() ---------------------------------

    def handoff(self, since_setup_ms: int, continues_retained_clock: bool = True) -> None:
        at = self.millis(since_setup_ms)
        elapsed = self.scheduler.handoff_elapsed(
            continues_retained_clock,
            self.session_ms,
            u32(at - BOOTLOADER_MS),
            at,
        )
        self.session_ms = elapsed
        self.interval_start_ms = elapsed
        self.handoff_at_ms = at


START_MS = 1_000_000  # session time at this wake's setup() entry


def released_board(
    scheduler: Scheduler, session_ms: int, gap_ms: int, cycle_entry_ms: int = 0
) -> tuple[Board, Plan]:
    """A timer wake claimed at its rendezvous, handed back `session_ms` after
    setup() entry, and put to sleep `gap_ms` after the handoff."""

    board = Board(
        scheduler,
        session_ms=START_MS,
        interval_start_ms=START_MS - NOMINAL_MS,
        cycle_entry_ms=cycle_entry_ms,
    )
    board.handoff(since_setup_ms=session_ms)
    return board, board.sleep("AUTO_SLEEP_HOST_RELEASE", since_setup_ms=session_ms + gap_ms)


# ============================================================================
# Executed behavior
# ============================================================================


def test_unclaimed_wakes_keep_the_configured_cadence(scheduler):
    """Five unclaimed wakes in a row: each record reports one cadence plus its
    read time, and each wake starts exactly one cadence after the last reset.

    `cycle_entry_ms` is arbitrary. The wake cycle and the scheduler measure
    from different instants, and the difference cancels from one wake to the
    next, so it never reaches `interval_ms`.
    """

    board = Board(
        scheduler,
        session_ms=START_MS,
        interval_start_ms=START_MS - NOMINAL_MS,
        cycle_entry_ms=40,
    )
    previous_session = -1

    for wake in range(5):
        record = board.wake_record(since_setup_ms=40 + READ_MS)
        board.wake_reset(since_setup_ms=40 + RESET_MS)
        boundary = board.interval_start_ms
        plan = board.sleep("AUTO_SLEEP_TIMER_WAKE", since_setup_ms=10_400)

        if wake > 0:
            assert record["interval_ms"] == NOMINAL_MS + READ_MS

        assert record["session_elapsed_ms"] > previous_session
        previous_session = record["session_elapsed_ms"]

        assert not plan.overran
        assert plan.interval_start_ms == boundary
        assert plan.wake_elapsed_ms == boundary + NOMINAL_MS


# gap: time from the handoff to the scheduler. RELEASE waits out the 250 ms
# grace; lease expiry sleeps directly, after only the handoff's own output.
HANDBACKS = (
    pytest.param(GRACE_MS, id="release"),
    pytest.param(4, id="lease-expiry"),
)

# How long the host held the board, measured from setup() entry to the
# handoff. 75 s passes one cadence; 3 h passes micros()'s 71.6-minute wrap.
SESSIONS = (
    pytest.param(28_000, id="28s"),
    pytest.param(75_000, id="75s"),
    pytest.param(3 * 3_600_000, id="3h"),
)


@pytest.mark.parametrize("gap_ms", HANDBACKS)
@pytest.mark.parametrize("session_ms", SESSIONS)
def test_a_handoff_sleeps_the_rest_of_one_cadence_exactly_once(
    scheduler, session_ms, gap_ms
):
    """D-034: the handoff is a new interval boundary. The first sleep after it
    is one cadence minus only the time since the handoff.

    Pre-fix, the time since setup() entry was added on top: 31,750 ms for the
    28 s RELEASE. At 75 s and 3 h the doubled time passed the deadline, so the
    board printed a false overrun warning, moved the boundary, and carried the
    session length in its session clock for the rest of the boot.
    """

    board, plan = released_board(scheduler, session_ms, gap_ms)

    assert plan.sleep_ms == NOMINAL_MS - gap_ms
    assert not plan.overran, "a handoff is a fresh boundary; it cannot be overdue"
    assert plan.interval_start_ms == START_MS + session_ms
    assert plan.wake_elapsed_ms == START_MS + session_ms + NOMINAL_MS

    # D-033: continues the retained clock, counting the session exactly once.
    record = board.wake_record(since_setup_ms=READ_MS)
    assert record["session_elapsed_ms"] == START_MS + session_ms + NOMINAL_MS + READ_MS


@pytest.mark.parametrize("gap_ms", HANDBACKS)
@pytest.mark.parametrize("session_ms", SESSIONS)
def test_the_first_record_after_a_handoff_reports_the_time_that_passed(
    scheduler, session_ms, gap_ms
):
    """The next record's `interval_ms` against the time that actually passed
    between the handoff's accumulator reset and the next wake's read.

    They differ only by the time before the next wake cycle starts its clock:
    the bootloader, and setup() up to runAutonomousWakeCycle(), arbitrarily
    40 ms here. Both are milliseconds and both predate this fix: session time
    has never included the bootloader, and on an unclaimed wake the setup()
    part cancels (test above). Pre-fix, the 28 s RELEASE record claimed
    60,150 ms for about 32.5 s that really passed.
    """

    board, plan = released_board(scheduler, session_ms, gap_ms, cycle_entry_ms=40)
    record = board.wake_record(since_setup_ms=40 + READ_MS)

    really_passed = gap_ms + plan.sleep_ms + BOOTLOADER_MS + 40 + READ_MS
    assert record["interval_ms"] == NOMINAL_MS + READ_MS
    assert really_passed - record["interval_ms"] == BOOTLOADER_MS + 40


@pytest.mark.parametrize(
    ("timer_wake", "rtc_valid"),
    [
        pytest.param(False, False, id="cold-boot-claimed-in-maintenance-window"),
        pytest.param(True, False, id="timer-wake-whose-rtc-state-was-rebuilt"),
    ],
)
def test_a_session_this_boot_began_runs_on_this_boots_clock(
    scheduler, timer_wake, rtc_valid
):
    """No retained clock to continue: the handoff's rebuild starts a new boot_id,
    and the session clock is millis().

    Pre-fix, the cold-boot case double-counted too, since millis() is itself
    the time since boot: a release 45 s after setup() entry slept 14,750 ms.
    The rebuilt case also tested the magic after the rebuild had set it, and
    so would have continued a retained clock just declared invalid.
    """

    stale = 3_000_000_000  # whatever RTC memory held; must not survive
    board = Board(scheduler, session_ms=stale, interval_start_ms=stale)
    board.handoff(since_setup_ms=45_000, continues_retained_clock=timer_wake and rtc_valid)

    assert board.session_ms == BOOTLOADER_MS + 45_000

    plan = board.sleep("AUTO_SLEEP_HOST_RELEASE", since_setup_ms=45_000 + GRACE_MS)
    assert plan.sleep_ms == NOMINAL_MS - GRACE_MS
    assert plan.wake_elapsed_ms == BOOTLOADER_MS + 45_000 + NOMINAL_MS


def test_the_session_clock_rolls_over_without_breaking_the_deadline(scheduler):
    """uint32 session time wraps after 49.7 days; `(int32_t)(target - now)`
    exists so the deadline survives that. Here the handoff lands 5 s before
    the wrap, so the deadline is past it and `now` is not."""

    near_wrap = 2**32 - 28_000 - 5_000
    board = Board(scheduler, session_ms=near_wrap, interval_start_ms=near_wrap - NOMINAL_MS)
    board.handoff(since_setup_ms=28_000)
    plan = board.sleep("AUTO_SLEEP_HOST_RELEASE", since_setup_ms=28_000 + GRACE_MS)

    assert board.interval_start_ms == 2**32 - 5_000
    assert not plan.overran
    assert plan.sleep_ms == NOMINAL_MS - GRACE_MS
    assert plan.wake_elapsed_ms == NOMINAL_MS - 5_000


@pytest.mark.parametrize("overdue_ms", [0, 1, 220, 45_000, 10 * NOMINAL_MS])
def test_an_overdue_deadline_sleeps_one_full_cadence(scheduler, overdue_ms):
    """D-032 as designed: at or past the deadline, warn and sleep one full
    cadence. Never an immediate wake, never an unsigned underflow into a
    ~49-day sleep. Exactly at the deadline counts as overdue: the test is `> 0`.

    Where the boundary goes on an overrun is deliberately NOT pinned here. D-032
    moves it to now without an accumulator reset, which is the defect the
    strict xfail below records; pinning it would freeze that defect (D-043).
    """

    start = 5_000_000
    now = start + NOMINAL_MS + overdue_ms
    plan = scheduler.plan(start, now, NOMINAL_MS)

    assert plan.overran
    assert plan.overrun_ms == overdue_ms
    assert plan.sleep_ms == NOMINAL_MS
    assert plan.wake_elapsed_ms == now + NOMINAL_MS


@pytest.mark.parametrize("start", [0, 5_000_000, 2**31, 2**32 - 30_000])
@pytest.mark.parametrize("offset", [0, 1, 30_000, NOMINAL_MS - 1, NOMINAL_MS, 90_000])
def test_a_sleep_is_never_zero_and_never_longer_than_one_cadence(
    scheduler, start, offset
):
    """Across the uint32 rollover, before and after the deadline."""

    now = u32(start + offset)
    plan = scheduler.plan(start, now, NOMINAL_MS)

    assert 0 < plan.sleep_ms <= NOMINAL_MS
    assert plan.wake_elapsed_ms == u32(now + plan.sleep_ms)
    assert plan.overran == (offset >= NOMINAL_MS)

    if not plan.overran:
        assert plan.sleep_ms == NOMINAL_MS - offset
        assert plan.interval_start_ms == start


@pytest.mark.xfail(
    strict=True,
    reason=(
        "FOUND 2026-09-18, not fixed: an overrun moves the interval boundary "
        "without resetting the accumulators (docs/BACKLOG.md)"
    ),
)
def test_after_an_overrun_the_next_record_covers_what_its_charge_covers(scheduler):
    """D-018: a record's `interval_ms` is the time its charge accumulated over.

    At LOGGER INTERVAL 10, the minimum, every unclaimed wake overruns: the
    10-second rendezvous starts after the accumulator reset. The overrun moves
    rtcAutoIntervalStartMs to the moment of sleeping, but nothing resets the
    accumulators there, so the next record's charge still runs from the wake's
    reset. Today it claims 10,150 ms for about 20,370 ms of charge, and it is
    not flagged INTERVAL_ODD. Unreachable at the 60 s default.
    """

    board = Board(
        scheduler,
        session_ms=START_MS,
        interval_start_ms=START_MS - 10_000,
        nominal_ms=10_000,
    )
    board.wake_reset(since_setup_ms=RESET_MS)
    charge_since = board.interval_start_ms

    plan = board.sleep("AUTO_SLEEP_TIMER_WAKE", since_setup_ms=10_400)
    assert plan.overran, "the premise: a 10 s rendezvous overruns a 10 s cadence"

    record = board.wake_record(since_setup_ms=READ_MS)
    assert record["interval_ms"] == record["session_elapsed_ms"] - charge_since


def test_each_path_takes_the_clock_its_retained_value_was_set_for(scheduler):
    """autonomousElapsedAtSleepMs(), path by path, with every input distinct so
    a wrong choice cannot pass by coincidence."""

    retained, awake, since_handoff, boot_millis = 7_000_000, 28_250, 250, 28_550
    expected = {
        "AUTO_SLEEP_TIMER_WAKE": retained + awake,
        "AUTO_SLEEP_HOST_RELEASE": retained + since_handoff,
        "AUTO_SLEEP_COLD_BOOT": boot_millis,
        "AUTO_SLEEP_ARM_COMMAND": boot_millis,
        "AUTO_SLEEP_RTC_LOST": boot_millis,
    }

    assert set(sleep_paths(firmware_source.load())) == set(expected), (
        "a sleep path was added or renamed; decide which clock it continues"
    )

    for path, value in expected.items():
        actual = scheduler.elapsed_at_sleep(path, retained, awake, since_handoff, boot_millis)
        assert actual == value, path


def test_the_handoff_continues_the_retained_clock_only_when_told_to(scheduler):
    assert scheduler.handoff_elapsed(True, 7_000_000, 28_000, 28_300) == 7_028_000
    assert scheduler.handoff_elapsed(False, 7_000_000, 28_000, 28_300) == 28_300


# ============================================================================
# The shipped source composes those functions the way Board does
# ============================================================================


@pytest.fixture(scope="module")
def sketch() -> firmware_source.Sketch:
    return firmware_source.load()


def test_source_scheduler_measures_each_path_from_its_own_instant(sketch):
    """autonomousDeepSleepAgain() as Board.sleep() models it."""

    body = sketch.function("autonomousDeepSleepAgain")

    for statement in (
        "const uint32_t awakeUs = micros() - setupEntryMicros;",
        "const uint32_t awakeMs = awakeUs / 1000UL;",
        "const uint32_t nowMs = millis();",
        "const uint32_t currentElapsedMs = autonomousElapsedAtSleepMs("
        " path, rtcAutoSessionElapsedMs, awakeMs, nowMs - hostHandoffAtMs, nowMs);",
        "const uint32_t nominalMs = autonomousIntervalSeconds * 1000UL;",
        "const AutoSleepPlan plan ="
        " planAutonomousSleep(rtcAutoIntervalStartMs, currentElapsedMs, nominalMs);",
        "const uint32_t sleepMs = plan.sleepMs;",
        "rtcAutoIntervalStartMs = plan.intervalStartMs;",
        "rtcAutoSessionElapsedMs = plan.wakeElapsedMs;",
        "const uint64_t sleepUs = static_cast<uint64_t>(sleepMs) * 1000ULL;",
        "esp_sleep_enable_timer_wakeup(sleepUs);",
    ):
        assert body.contains(statement), f"autonomousDeepSleepAgain(): `{statement}`"

    # The retained clock is written once, from the plan, and never re-derived
    # from the awake time.
    assert body.count("rtcAutoSessionElapsedMs =") == 1
    assert not body.contains("rtcAutoSessionElapsedMs + awakeMs")


def test_source_handoff_sets_the_clock_and_the_instant_together(sketch):
    """beginAutonomousSleepFromHostSession() as Board.handoff() models it."""

    body = sketch.function("beginAutonomousSleepFromHostSession")

    decided = body.index(
        "const bool continuesRetainedClock ="
        " bootWakeupCause == ESP_SLEEP_WAKEUP_TIMER && rtcAutoMagic == AUTO_RTC_MAGIC;"
    )
    assert decided < body.index("if (rtcAutoMagic != AUTO_RTC_MAGIC)"), (
        "whether the retained clock continues must be decided before the rebuild "
        "sets the magic"
    )

    sequence = (
        "const uint32_t handoffAtMs = millis();",
        "const uint32_t sessionElapsedNow = autonomousHandoffElapsedMs("
        " continuesRetainedClock, rtcAutoSessionElapsedMs,"
        " handoffAtMs - setupEntryMicros / 1000UL, handoffAtMs);",
        "rtcAutoSessionElapsedMs = sessionElapsedNow;",
        "rtcAutoIntervalStartMs = sessionElapsedNow;",
        "hostHandoffAtMs = handoffAtMs;",
        "if (sleepNow)",
    )
    positions = [body.index(statement) for statement in sequence]
    assert positions == sorted(positions), "handoff timing statements out of order"

    # micros() wraps after 71.6 minutes, well inside a logger session.
    assert not body.contains("micros ("), "the handoff must measure with millis()"


def test_source_only_the_expected_code_writes_the_timing_state(sketch):
    """Who may move each timing value. A new writer has to justify itself here."""

    assert sketch.functions_containing("hostHandoffAtMs =") == {
        "beginAutonomousSleepFromHostSession",
    }
    assert sketch.functions_containing("hostHandoffAtMs") == {
        "beginAutonomousSleepFromHostSession",
        "autonomousDeepSleepAgain",
    }
    assert sketch.functions_containing("rtcAutoSessionElapsedMs =") == {
        "autonomousDeepSleepAgain",  # the commanded clock for the next wake
        "beginAutonomousSleepFromHostSession",  # the handoff
        "armAutonomousTest",  # a session begun on this boot
        "setup",  # cold-boot resume, RTC-loss rebuild
    }
    assert sketch.functions_containing("rtcAutoIntervalStartMs =") == {
        "runAutonomousWakeCycle",
        "autonomousDeepSleepAgain",
        "beginAutonomousSleepFromHostSession",
        "armAutonomousTest",
        "setup",
    }


def test_source_wake_cycle_measures_the_record_as_modeled(sketch):
    """runAutonomousWakeCycle() as Board.wake_record() and wake_reset() model it."""

    body = sketch.function("runAutonomousWakeCycle")

    for statement in (
        "const uint32_t t0 = micros();",
        "const uint32_t nowElapsedMs = rtcAutoSessionElapsedMs + (micros() - t0) / 1000UL;",
        "uint32_t intervalMs = nowElapsedMs - rtcAutoIntervalStartMs;",
        "record.session_elapsed_ms = nowElapsedMs;",
        "record.interval_ms = intervalMs;",
        "rtcAutoIntervalStartMs = rtcAutoSessionElapsedMs + (micros() - t0) / 1000UL;",
        "autonomousDeepSleepAgain(AUTO_SLEEP_TIMER_WAKE);",
    ):
        assert body.contains(statement), f"runAutonomousWakeCycle(): `{statement}`"

    assert sketch.code.contains("constexpr uint32_t SESSION_RELEASE_ACK_GRACE_MS = 250;")
    assert sketch.code.contains("constexpr uint32_t AUTO_INTERVAL_SECONDS_DEFAULT = 60;")


def test_source_timing_functions_are_host_faithful(sketch):
    """The host run only means something if the host computes what the ESP32
    does. `unsigned long` is 64 bits on the host and 32 on the ESP32-C3, so
    the functions and their struct may use fixed-width types only."""

    pieces = [sketch.type_definition("struct", "AutoSleepPlan")] + [
        sketch.definition(name, returns) for name, returns in HELPERS
    ]

    for piece in pieces:
        assert "long" not in piece.texts, f"`long` in: {piece}"

        for token in piece.tokens:
            if token.kind == "number":
                assert not token.text.upper().endswith("L"), (
                    f"{token.text} at {token.where} is a long literal"
                )
