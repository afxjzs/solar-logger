"""Machine-readable telemetry: the CSV lines the host parser consumes.

Written on 2026-09-23 against the monolith, before the CSV formatting moved
into its own module, and meant to keep asserting the same bytes afterwards
(D-043).

EXECUTED
========

The formatting code is pure Serial output, so these tests take its real source
out of the sketch through tests/firmware_source.py - wherever it lives -
compile it on the host against a Serial that records what is printed, and run
it. Every line asserted below was produced by the firmware's own code.

THE GOLDEN LINES ARE THE CONTRACT
=================================

`GOLDEN` holds the exact bytes the firmware emitted for `INPUTS` before the
telemetry extraction. It is the frozen wire format: prefixes, field order,
field count, delimiter and per-field precision. An extraction may move this
code and may turn a global into a parameter; it may not change a byte of
`GOLDEN`. A diff that touches `GOLDEN` or `INPUTS` is a behavior change and
needs its own reason.

The harness below - which names the functions and how they are called - does
follow the code, because that is what an extraction moves.

WHAT THE HOST DOES WITH THEM
============================

app/solar_logger.py is run against the same lines, through its real parsers,
so a change to firmware field order fails here rather than in a CSV file.

ONE LIMIT, STATED
=================

`RecordingSerial::printFloat` is a copy of `Print::printFloat` from the ESP32
core (3.3.11, cores/esp32/Print.cpp), not Arduino's own object, so these are
the bytes that algorithm produces on the host. It differs from the device only
where `unsigned long` width matters, which is beyond the `ovf` cutoff that both
copies apply at 4294967040.0. Every value in `INPUTS` is far inside it.
"""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest

import firmware_source

import solar_logger


# ============================================================================
# The frozen contract
# ============================================================================

# Real values: experiment 3's checkpoint as the board reported it on
# 2026-09-23, and one plausible interval around it. The key is the @NAME@
# placeholder the harness substitutes; the comment beside it is what the
# firmware calls the value.
INPUTS = {
    "EXPERIMENT_ID": 3,
    "COMPLETED_INTERVAL": 53,
    "RUNNING_CHARGE": 6.068421,
    "RUNNING_ENERGY": 83.565454,
    "VOLTAGE_V": 13.108789,
    "CURRENT_MA": 337.584,
    "POWER_MW": 4425.3568,
    "TEMPERATURE_C": 28.15625,
    "ELAPSED_SECONDS": 60.003,
    "INTERVAL_CHARGE": 0.005628,
    "INTERVAL_ENERGY": 0.073765,
    "AVERAGE_CURRENT": 337.68,
    "AVERAGE_POWER": 4425.9,
    "SLEEP_TEST_CYCLE": 7,
    # printLiveSample() derives elapsed_seconds itself from these two.
    "MILLIS": 187200,
    "INTERVAL_START_MS": 165000,
}

# The file every machine-readable line is built in. Before the telemetry
# extraction that is the sketch itself; afterwards it is the module.
TELEMETRY_SOURCE = "Arduino/solar-logger/telemetry.cpp"

CSV_HEADER_LINE = (
    "CSV_HEADER,"
    "experiment_id,"
    "interval,"
    "elapsed_seconds,"
    "voltage_V,"
    "current_mA,"
    "power_mW,"
    "temperature_C,"
    "interval_charge_mAh,"
    "interval_energy_mWh,"
    "average_current_mA,"
    "average_power_mW,"
    "running_charge_mAh,"
    "running_energy_mWh"
)

GOLDEN = {
    "header": [
        "",
        "[CSV] Machine-readable interval logging enabled.",
        CSV_HEADER_LINE,
    ],
    "start": ["CSV_EVENT,EXPERIMENT_START,3"],
    "resume": ["CSV_EVENT,EXPERIMENT_RESUME,3,53"],
    "sleep": ["CSV_EVENT,SLEEP_TEST_SLEEP,3,7"],
    "sample": [
        "CSV_SAMPLE,3,3202.200,13.108789,337.584000,4425.356800,28.1563"
    ],
    "interval": [
        "CSV_DATA,3,53,60.003,13.108789,337.584000,4425.356800,28.1563,"
        "0.005628000,0.073765000,337.680000,4425.900000,"
        "6.068421000,83.565454000"
    ],
}


# ============================================================================
# The harness: the firmware's own telemetry code, compiled for the host
# ============================================================================

# Print::printFloat is copied verbatim from the ESP32 core so that a double
# formats here exactly as it does on the board. Arduino does not use printf:
# it adds half an ulp of the requested precision and then truncates one digit
# at a time, which rounds differently from "%.*f" on ties.
PRELUDE = """\
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

struct RecordingSerial
{
	std::string text;

	void print(const char *value) { text += value; }
	void print(char value) { text += value; }
	void print(unsigned int value) { text += std::to_string(value); }
	void print(unsigned long value) { text += std::to_string(value); }
	void print(double value, int digits) { printFloat(value, digits); }

	void println() { text += '\\n'; }
	void println(const char *value) { text += value; text += '\\n'; }
	void println(unsigned int value) { print(value); text += '\\n'; }
	void println(unsigned long value) { print(value); text += '\\n'; }
	void println(double value, int digits)
	{
		printFloat(value, digits);
		text += '\\n';
	}

	// Print::printFloat, esp32 core 3.3.11, cores/esp32/Print.cpp.
	void printFloat(double number, uint8_t digits)
	{
		if (std::isnan(number)) { text += "nan"; return; }
		if (std::isinf(number)) { text += "inf"; return; }
		if (number > 4294967040.0) { text += "ovf"; return; }
		if (number < -4294967040.0) { text += "ovf"; return; }

		if (number < 0.0)
		{
			text += '-';
			number = -number;
		}

		double rounding = 0.5;
		for (uint8_t i = 0; i < digits; ++i) { rounding /= 10.0; }
		number += rounding;

		unsigned long int_part = (unsigned long)number;
		double remainder = number - (double)int_part;
		text += std::to_string(int_part);

		if (digits > 0) { text += '.'; }

		while (digits-- > 0)
		{
			remainder *= 10.0;
			int toPrint = int(remainder);
			text += std::to_string(toPrint);
			remainder -= toPrint;
		}
	}
};

static RecordingSerial Serial;

// What printLiveSample() reaches for beyond the formatting itself. The values
// come from INPUTS, so the elapsed_seconds it derives is the firmware's own
// arithmetic rather than a number this test chose.
struct SensorReading
{
	double voltage_V;
	double current_mA;
	double power_mW;
	double temperature_C;

	int32_t rawVshuntCounts;
	double shuntVoltage_mV;
	double currentFromShunt_mA;
};

static bool sleepPowerTestRunning = false;
static unsigned long intervalStartMs = @INTERVAL_START_MS@;
static unsigned long millis() { return @MILLIS@; }

static SensorReading theReading()
{
	SensorReading reading = {};
	reading.voltage_V = @VOLTAGE_V@;
	reading.current_mA = @CURRENT_MA@;
	reading.power_mW = @POWER_MW@;
	reading.temperature_C = @TEMPERATURE_C@;
	return reading;
}

static bool readSensor(SensorReading &reading, bool)
{
	reading = theReading();
	return true;
}

static uint32_t experimentId = @EXPERIMENT_ID@;
static uint32_t completedInterval = @COMPLETED_INTERVAL@;
static double runningCharge_mAh = @RUNNING_CHARGE@;
static double runningEnergy_mWh = @RUNNING_ENERGY@;
"""

DRIVER = r"""
int main(int argc, char **argv)
{
	for (int index = 1; index < argc; ++index)
	{
		const char *step = argv[index];
		Serial.text.clear();

		if (std::strcmp(step, "header") == 0)
		{
			@EMIT_HEADER@;
		}
		else if (std::strcmp(step, "start") == 0)
		{
			@EMIT_START@;
		}
		else if (std::strcmp(step, "resume") == 0)
		{
			@EMIT_RESUME@;
		}
		else if (std::strcmp(step, "sleep") == 0)
		{
			@EMIT_SLEEP@;
		}
		else if (std::strcmp(step, "sample") == 0)
		{
			printLiveSample();
		}
		else if (std::strcmp(step, "interval") == 0)
		{
			@EMIT_INTERVAL@;
		}
		else
		{
			std::fprintf(stderr, "unknown step: %s\n", step);
			return 2;
		}

		std::printf("STEP %s\n%sEND\n", step, Serial.text.c_str());
	}

	return 0;
}
"""

# How the sketch reaches each line today. The extraction changes these; it may
# not change GOLDEN.
EMIT = {
    "HEADER": "telemetryPrintHeader()",
    "START": "telemetryPrintExperimentStart(experimentId)",
    "RESUME": "telemetryPrintExperimentResume(experimentId, completedInterval)",
    "SLEEP": (
        'telemetryPrintSleepTestEvent("SLEEP_TEST_SLEEP", experimentId, '
        "@SLEEP_TEST_CYCLE@)"
    ),
    "INTERVAL": (
        "telemetryPrintInterval(TelemetryInterval{"
        "experimentId, completedInterval, @ELAPSED_SECONDS@, "
        "@VOLTAGE_V@, @CURRENT_MA@, @POWER_MW@, @TEMPERATURE_C@, "
        "@INTERVAL_CHARGE@, @INTERVAL_ENERGY@, "
        "@AVERAGE_CURRENT@, @AVERAGE_POWER@, "
        "runningCharge_mAh, runningEnergy_mWh})"
    ),
}

# The row structs the module's signatures need, taken from telemetry.h.
STRUCTS = ("TelemetrySample", "TelemetryInterval")

# The definitions the harness compiles, in dependency order.
FUNCTIONS = (
    ("telemetryPrintHeader", "void"),
    ("telemetryPrintSample", "void"),
    ("telemetryPrintInterval", "void"),
    ("telemetryPrintExperimentStart", "void"),
    ("telemetryPrintExperimentResume", "void"),
    ("telemetryPrintSleepTestEvent", "void"),
    ("printLiveSample", "void"),
)


def substituted(text: str) -> str:
    """Replace every @NAME@ placeholder with its value from INPUTS.

    An unreplaced placeholder is a typo that would otherwise reach the
    compiler as a stray identifier, so it fails here by name.
    """

    for name, value in INPUTS.items():
        text = text.replace(f"@{name}@", repr(value))

    leftover = [word for word in text.split("@")[1::2] if word.isupper()]
    assert not leftover, f"unreplaced placeholders: {sorted(set(leftover))}"
    return text


def harness_source(sketch: firmware_source.Sketch) -> str:
    driver = DRIVER

    for slot, call in EMIT.items():
        driver = driver.replace(f"@EMIT_{slot}@", call)

    extracted = [
        *(str(sketch.type_definition("struct", name)) for name in STRUCTS),
        *(str(sketch.definition(name, returns)) for name, returns in FUNCTIONS),
    ]
    return substituted("\n\n".join([PRELUDE, *extracted, driver]))


class Telemetry:
    """Runs a sequence of steps and returns what each one printed."""

    def __init__(self, binary: Path):
        self.binary = binary

    def run(self, *steps: str) -> dict[str, list[str]]:
        result = subprocess.run(
            [str(self.binary), *steps], capture_output=True, text=True, check=False
        )

        if result.returncode != 0:
            raise RuntimeError(
                f"harness {steps} exited {result.returncode}: {result.stderr}"
            )

        printed: dict[str, list[str]] = {}
        lines = result.stdout.split("\n")

        while lines and lines[0]:
            header = lines.pop(0)
            assert header.startswith("STEP "), header
            body: list[str] = []

            while lines[0] != "END":
                body.append(lines.pop(0))

            lines.pop(0)
            printed[header[len("STEP "):]] = body

        return printed


@pytest.fixture(scope="module")
def telemetry(tmp_path_factory: pytest.TempPathFactory) -> Telemetry:
    compiler = shutil.which("c++")

    if compiler is None:
        pytest.fail(
            "No host C++ compiler (`c++`) on PATH. These tests compile the "
            "firmware's telemetry code and run it; without a compiler the wire "
            "format has no executed cover, so this is a failure, not a skip."
        )

    source = harness_source(firmware_source.load())
    build = tmp_path_factory.mktemp("telemetry")
    cpp = build / "telemetry_harness.cpp"
    binary = build / "telemetry_harness"
    cpp.write_text(source, encoding="utf-8")

    result = subprocess.run(
        [compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-o", str(binary), str(cpp)],
        capture_output=True,
        text=True,
        check=False,
    )

    if result.returncode != 0:
        pytest.fail(
            "The firmware's telemetry code did not compile on the host:\n"
            f"{result.stderr}\n--- harness source ---\n{source}"
        )

    return Telemetry(binary)


@pytest.fixture(scope="module")
def emitted(telemetry: Telemetry) -> dict[str, list[str]]:
    return telemetry.run(*GOLDEN)


# ============================================================================
# The golden lines
# ============================================================================


@pytest.mark.parametrize("step", list(GOLDEN))
def test_each_line_is_byte_for_byte_what_it_was(step, emitted):
    """The whole point of the module: these bytes do not change when the code
    that writes them moves."""

    assert emitted[step] == GOLDEN[step]


def test_csv_header_names_the_fields_csv_data_carries_in_order():
    """CSV_HEADER describes CSV_DATA, so the two orders are one fact."""

    header = CSV_HEADER_LINE.split(",")[1:]
    row = GOLDEN["interval"][0].split(",")[1:]

    assert header == solar_logger.INTERVAL_FIRMWARE_COLUMNS
    assert len(row) == len(header)


def test_every_field_keeps_its_own_precision():
    """Precision is per field, not per row: seconds 3, volts/amps/watts 6,
    temperature 4, and charge/energy 9 because an interval's share of a
    milliamp-hour is small."""

    fields: dict[str, str] = dict(
        zip(CSV_HEADER_LINE.split(",")[1:], GOLDEN["interval"][0].split(",")[1:])
    )

    def decimals(name: str) -> int:
        return len(fields[name].split(".")[1])

    assert decimals("elapsed_seconds") == 3
    assert decimals("temperature_C") == 4

    for name in ("voltage_V", "current_mA", "power_mW",
                 "average_current_mA", "average_power_mW"):
        assert decimals(name) == 6, name

    for name in ("interval_charge_mAh", "interval_energy_mWh",
                 "running_charge_mAh", "running_energy_mWh"):
        assert decimals(name) == 9, name

    assert "." not in fields["experiment_id"]
    assert "." not in fields["interval"]


def test_a_sample_carries_the_six_fields_the_host_expects():
    sample = GOLDEN["sample"][0].split(",")

    assert sample[0] == "CSV_SAMPLE"
    assert len(sample) == len(solar_logger.SAMPLE_FIRMWARE_COLUMNS) + 1


def test_elapsed_seconds_is_completed_intervals_plus_time_into_this_one():
    """53 closed intervals of 60 s, plus 22.2 s since the accumulators were
    reset. The firmware derives it; this pins the meaning."""

    sample = dict(
        zip(solar_logger.SAMPLE_FIRMWARE_COLUMNS, GOLDEN["sample"][0].split(",")[1:])
    )
    expected = (
        INPUTS["COMPLETED_INTERVAL"] * 60.0
        + (INPUTS["MILLIS"] - INPUTS["INTERVAL_START_MS"]) / 1000.0
    )

    assert float(sample["elapsed_seconds"]) == pytest.approx(expected, abs=5e-4)


def test_an_event_names_itself_then_its_experiment():
    """Every CSV_EVENT is `CSV_EVENT,<type>,<experiment_id>` and then a tail the
    host keeps whole. EXPERIMENT_START has no tail, which is why the host's
    minimum is three fields and not four."""

    for step in ("start", "resume", "sleep"):
        parts = GOLDEN[step][0].split(",")
        assert parts[0] == "CSV_EVENT"
        assert parts[1] and parts[1].isupper()
        assert parts[2] == str(INPUTS["EXPERIMENT_ID"])

    assert len(GOLDEN["start"][0].split(",")) == 3
    assert len(GOLDEN["resume"][0].split(",")) == 4
    assert len(GOLDEN["sleep"][0].split(",")) == 4


# ============================================================================
# The host parses exactly these lines
# ============================================================================


def test_the_host_parses_the_interval_row_it_is_sent():
    row = solar_logger.parse_interval(GOLDEN["interval"][0])

    assert row is not None
    assert row["experiment_id"] == INPUTS["EXPERIMENT_ID"]
    assert row["interval"] == INPUTS["COMPLETED_INTERVAL"]
    assert row["elapsed_seconds"] == pytest.approx(INPUTS["ELAPSED_SECONDS"])
    assert row["voltage_V"] == pytest.approx(INPUTS["VOLTAGE_V"])
    assert row["running_charge_mAh"] == pytest.approx(INPUTS["RUNNING_CHARGE"])
    assert row["running_energy_mWh"] == pytest.approx(INPUTS["RUNNING_ENERGY"])
    assert list(row) == solar_logger.INTERVAL_FIRMWARE_COLUMNS


def test_the_host_parses_the_sample_row_it_is_sent():
    row = solar_logger.parse_sample(GOLDEN["sample"][0])

    assert row is not None
    assert row["experiment_id"] == INPUTS["EXPERIMENT_ID"]
    assert row["current_mA"] == pytest.approx(INPUTS["CURRENT_MA"])
    assert list(row) == solar_logger.SAMPLE_FIRMWARE_COLUMNS


@pytest.mark.parametrize(
    "step,event_type,detail",
    [
        ("start", "EXPERIMENT_START", ""),
        ("resume", "EXPERIMENT_RESUME", "53"),
        ("sleep", "SLEEP_TEST_SLEEP", "7"),
    ],
)
def test_the_host_parses_each_event_it_is_sent(step, event_type, detail):
    event = solar_logger.parse_event(GOLDEN[step][0])

    assert event is not None
    assert event["event_type"] == event_type
    assert event["experiment_id"] == INPUTS["EXPERIMENT_ID"]
    assert event["detail"] == detail


def test_the_host_would_notice_a_changed_field_count():
    """The guard that makes the two halves of this test worth having: a row with
    one field too few is refused rather than written short."""

    short = ",".join(GOLDEN["interval"][0].split(",")[:-1])

    assert solar_logger.parse_interval(short) is None
    assert solar_logger.parse_sample(",".join(GOLDEN["sample"][0].split(",")[:-1])) is None


def test_the_csv_files_the_logger_opens_carry_the_firmware_fields():
    """captured_at is the host's, and every other column is the firmware's, in
    the firmware's order (D-004)."""

    assert solar_logger.SAMPLE_COLUMNS == ["captured_at", *solar_logger.SAMPLE_FIRMWARE_COLUMNS]
    assert solar_logger.INTERVAL_COLUMNS == [
        "captured_at",
        *solar_logger.INTERVAL_FIRMWARE_COLUMNS,
    ]
    assert solar_logger.EVENT_COLUMNS == [
        "captured_at",
        "event_type",
        "experiment_id",
        "detail",
    ]


# ============================================================================
# Where the lines are emitted from
# ============================================================================


def line_owners() -> dict[str, set[tuple[str, str]]]:
    """For each CSV prefix, the (function, file) pairs that build that line."""

    sketch = firmware_source.load()

    return {
        prefix: {
            # `where` is "<path>:<line>"; the line moves with any edit and the
            # path is the fact this test is about.
            (name, definition.where.rsplit(":", 1)[0])
            for name, definitions in sketch.functions.items()
            for definition in definitions
            # The token text keeps its quotes, and matching them is what makes
            # this safe against the escape sequences other functions contain.
            if any(token.startswith(f'"{prefix}') for token in definition.body.strings())
        }
        for prefix in ("CSV_HEADER,", "CSV_SAMPLE,", "CSV_DATA,", "CSV_EVENT,")
    }


def test_exactly_one_function_builds_each_machine_readable_line():
    """One place builds each line. A second Serial.print("CSV_...") anywhere in
    the firmware would be a second wire format nobody is maintaining.

    CSV_EVENT has three because the three event shapes differ: EXPERIMENT_START
    carries no tail, and the other two carry one value each.
    """

    owners = line_owners()

    assert len(owners["CSV_HEADER,"]) == 1
    assert len(owners["CSV_SAMPLE,"]) == 1
    assert len(owners["CSV_DATA,"]) == 1
    assert len(owners["CSV_EVENT,"]) == 3


def test_every_machine_readable_line_is_built_in_one_place():
    """The module boundary, as a fact about the source: every CSV_ line comes
    out of the same file. `TELEMETRY_SOURCE` is the one value the extraction
    changes, and it changes because the code moved, not because the bytes did.
    """

    where = {
        origin
        for owned in line_owners().values()
        for _name, origin in owned
    }

    assert where == {TELEMETRY_SOURCE}


def test_the_command_protocol_is_not_telemetry():
    """CMD_ACK and CMD_RESULT go through the bounded protocol writer with its
    own 100 ms deadline (D-036). Telemetry uses ordinary Serial output and may
    be dropped (D-027). Neither may acquire the other's delivery rules."""

    sketch = firmware_source.load()

    assert sketch.callers("writeProtocolLine") == {
        "emitCommandAck",
        "emitCommandResult",
    }

    for owned in line_owners().values():
        for name, _origin in owned:
            assert not sketch.function(name).contains("writeProtocolLine ("), name


def test_the_interval_close_emits_its_row_after_the_numbers_are_final():
    """CSV_DATA reports a closed interval, so it is printed after the averages
    are computed, and before the NVS checkpoint that may fail: the row is valid
    whether or not the checkpoint lands."""

    close = firmware_source.load().function("closeMeasurementInterval")
    emitted = close.index("telemetryPrintInterval (")

    # The running totals take this interval in, then the averages are derived,
    # then the row is built out of both.
    assert close.index("runningCharge_mAh +=") < close.index("double averageCurrent_mA =")
    assert close.index("double averageCurrent_mA =") < close.index("const TelemetryInterval row =")
    assert close.index("const TelemetryInterval row =") < emitted
    assert emitted < close.index("saveCheckpoint (")


def test_a_deep_sleep_test_suspends_the_sample_row_and_nothing_else():
    """D-012: elapsed_seconds would sawtooth backwards across repeated deep
    sleeps, so the row is suppressed rather than emitted wrong. The check is
    sleepPowerTestRunning, not the broader accounting-suspended predicate."""

    sample = firmware_source.load().function("printLiveSample")

    assert sample.contains("if (sleepPowerTestRunning) { return; }")
    assert not sample.contains("intervalAccountingSuspended (")
