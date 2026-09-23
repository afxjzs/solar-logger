"""Connection bookkeeping: which transport a host has claimed (D-025).

Written on 2026-09-18 against the monolith, before the bookkeeping moved into
its own module, and meant to pass unchanged after the move (D-043).

EXECUTED
========

The connection code is pure bookkeeping plus Serial output, so the tests take
its real source out of the sketch through tests/firmware_source.py - wherever
it lives - compile it on the host with a Serial that records what is printed,
and run it. Every mask, answer and line of text asserted below comes from the
firmware's own code, including the exact diagnostic wording that the rendezvous
and STATUS output, and the hardware acceptance procedures, depend on.

SOURCE
======

Who claims and releases, and that sleep policy asks anyHostConnected() and
never the USB hardware, can only be read from the source. Those tests pin the
call sites, not board behavior; that is the hardware acceptance sequence in
docs/LAB_NOTES.md.
"""

from __future__ import annotations

import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

import pytest

import firmware_source

TRANSPORTS = ("USB", "WIFI", "BLE")

FUNCTIONS = (
    ("anyHostConnected", "bool"),
    ("printActiveTransports", "void"),
    ("transportName", "const char *"),
    ("connectionClaim", "void"),
    ("connectionRelease", "void"),
)


# ============================================================================
# The harness: the firmware's own connection code, compiled for the host
# ============================================================================

PRELUDE = """\
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// The only Serial calls the connection code makes, recorded instead of sent.
struct RecordingSerial
{
	std::string text;

	void print(const char *value) { text += value; }
	void println(const char *value)
	{
		text += value;
		text += '\\n';
	}
	void println() { text += '\\n'; }
};

static RecordingSerial Serial;
"""

DRIVER = r"""
static uint8_t transport_named(const char *name)
{
@TRANSPORTS@
	std::fprintf(stderr, "unknown transport: %s\n", name);
	std::exit(2);
}

int main(int argc, char **argv)
{
	for (int index = 1; index < argc; ++index)
	{
		const char *step = argv[index];
		Serial.text.clear();

		if (std::strcmp(step, "print") == 0)
		{
			printActiveTransports();
		}
		else if (std::strncmp(step, "claim:", 6) == 0)
		{
			connectionClaim(transport_named(step + 6));
		}
		else if (std::strncmp(step, "release:", 8) == 0)
		{
			connectionRelease(transport_named(step + 8));
		}
		else
		{
			std::fprintf(stderr, "unknown step: %s\n", step);
			return 2;
		}

		std::printf("STEP %s\n%sMASK %u ANY %s\nEND\n", step, Serial.text.c_str(),
								static_cast<unsigned>(activeTransports),
								anyHostConnected() ? "YES" : "NO");
	}

	return 0;
}
"""


def statement(code: firmware_source.Code, head: str) -> firmware_source.Code:
    """The one statement that starts with `head`, through its semicolon."""

    starts = code.find_all(head)
    assert len(starts) == 1, f"expected exactly one `{head}`, found {len(starts)}"
    end = code.texts.index(";", starts[0])
    return code.slice(starts[0], end + 1)


def constant(sketch: firmware_source.Sketch, name: str) -> firmware_source.Code:
    return statement(sketch.code, f"constexpr uint8_t {name} =")


def harness_source(sketch: firmware_source.Sketch) -> str:
    extracted = [
        *(str(constant(sketch, f"CONNECTION_{name}")) for name in ("NONE", *TRANSPORTS)),
        # `static` or not, the definition is the same statement.
        str(statement(sketch.code, "uint8_t activeTransports =")),
        *(str(sketch.definition(name, returns)) for name, returns in FUNCTIONS),
    ]
    transports = "\n".join(
        f'\tif (std::strcmp(name, "{name}") == 0)\n\t{{\n\t\treturn CONNECTION_{name};\n\t}}\n'
        for name in TRANSPORTS
    )
    return "\n\n".join([PRELUDE, *extracted, DRIVER.replace("@TRANSPORTS@", transports)])


@dataclass(frozen=True)
class Step:
    name: str
    printed: list[str]
    mask: int
    any_host: bool


class Connection:
    """Runs a sequence of steps through one fresh copy of the firmware state."""

    def __init__(self, binary: Path):
        self.binary = binary

    def run(self, *steps: str) -> list[Step]:
        result = subprocess.run(
            [str(self.binary), *steps], capture_output=True, text=True, check=False
        )

        if result.returncode != 0:
            raise RuntimeError(f"harness {steps} exited {result.returncode}: {result.stderr}")

        parsed: list[Step] = []
        lines = result.stdout.splitlines()

        while lines:
            header = lines.pop(0)
            assert header.startswith("STEP "), header
            printed: list[str] = []

            while not lines[0].startswith("MASK "):
                printed.append(lines.pop(0))

            _, mask, _, any_host = lines.pop(0).split()
            assert lines.pop(0) == "END"
            parsed.append(Step(header[len("STEP "):], printed, int(mask), any_host == "YES"))

        return parsed


@pytest.fixture(scope="module")
def connection(tmp_path_factory: pytest.TempPathFactory) -> Connection:
    compiler = shutil.which("c++")

    if compiler is None:
        pytest.fail(
            "No host C++ compiler (`c++`) on PATH. These tests compile the firmware's "
            "connection code and run it; without a compiler it has no executed cover, "
            "so this is a failure, not a skip."
        )

    source = harness_source(firmware_source.load())
    build = tmp_path_factory.mktemp("connection")
    cpp = build / "connection_harness.cpp"
    binary = build / "connection_harness"
    cpp.write_text(source, encoding="utf-8")

    result = subprocess.run(
        [compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-o", str(binary), str(cpp)],
        capture_output=True,
        text=True,
        check=False,
    )

    if result.returncode != 0:
        pytest.fail(
            "The firmware's connection code did not compile on the host:\n"
            f"{result.stderr}\n--- harness source ---\n{source}"
        )

    return Connection(binary)


def value(sketch: firmware_source.Sketch, name: str) -> int:
    return int(constant(sketch, name).texts[-2], 0)


# ============================================================================
# Executed behavior
# ============================================================================


def test_no_transport_is_active_until_one_is_claimed(connection):
    """A board with USB plugged in and a terminal open still starts here: only
    an explicit lease claims a transport (D-025)."""

    (step,) = connection.run("print")

    assert step.printed == ["[CONNECTION] Active transports: NONE"]
    assert step.mask == 0
    assert not step.any_host


def test_claiming_usb_connects_a_host(connection):
    (step,) = connection.run("claim:USB")

    assert step.printed == [
        "[CONNECTION] USB transport CLAIMED by an explicit host lease.",
        "[CONNECTION] Active transports: USB",
        "[CONNECTION] Any host connected: YES",
    ]
    assert step.mask == value(firmware_source.load(), "CONNECTION_USB")
    assert step.any_host


def test_releasing_usb_leaves_no_host_connected(connection):
    _, released = connection.run("claim:USB", "release:USB")

    assert released.printed == [
        "[CONNECTION] USB transport RELEASED.",
        "[CONNECTION] Active transports: NONE",
        "[CONNECTION] Any host connected: NO",
    ]
    assert released.mask == 0
    assert not released.any_host


def test_releasing_usb_leaves_every_other_transport_claimed(connection):
    """Nothing claims Wi-Fi or BLE yet. The bits exist so they can hold the
    board awake later, which only works if releasing one leaves the rest."""

    sketch = firmware_source.load()
    usb, wifi, ble = (value(sketch, f"CONNECTION_{name}") for name in TRANSPORTS)

    steps = connection.run(
        "claim:WIFI", "claim:BLE", "claim:USB", "print", "release:USB", "print"
    )

    assert [step.mask for step in steps] == [
        wifi,
        wifi | ble,
        usb | wifi | ble,
        usb | wifi | ble,
        wifi | ble,
        wifi | ble,
    ]
    assert steps[3].printed == ["[CONNECTION] Active transports: USB+WIFI+BLE"]
    assert steps[4].printed == [
        "[CONNECTION] USB transport RELEASED.",
        "[CONNECTION] Active transports: WIFI+BLE",
        "[CONNECTION] Any host connected: YES",
    ]
    assert steps[4].any_host

    # And the other way round: releasing another transport leaves USB claimed.
    *_, released_wifi = connection.run("claim:USB", "claim:WIFI", "release:WIFI")
    assert released_wifi.mask == usb
    assert released_wifi.any_host


def test_claim_and_release_say_nothing_when_nothing_changes(connection):
    usb = value(firmware_source.load(), "CONNECTION_USB")
    steps = connection.run("release:USB", "claim:USB", "claim:USB", "release:USB", "release:USB")

    assert [step.mask for step in steps] == [0, usb, usb, 0, 0]
    assert [bool(step.printed) for step in steps] == [False, True, False, True, False]


def test_each_transport_is_its_own_bit():
    sketch = firmware_source.load()
    bits = [value(sketch, f"CONNECTION_{name}") for name in TRANSPORTS]

    assert value(sketch, "CONNECTION_NONE") == 0
    assert all(bit and bit & (bit - 1) == 0 for bit in bits), bits
    assert len(set(bits)) == len(bits)


# ============================================================================
# Who claims, who releases, and who asks
# ============================================================================


def test_only_an_explicit_host_lease_claims_a_transport():
    """D-025: electrical presence is never a claim. HOLD is the one claim, and
    isPlugged() / isConnected() are only ever printed."""

    sketch = firmware_source.load()

    assert sketch.callers("connectionClaim") == {"hostSessionHold"}
    assert sketch.function("hostSessionHold").contains("connectionClaim(CONNECTION_USB);")

    assert sketch.functions_containing("activeTransports |=") == {"connectionClaim"}
    assert sketch.functions_containing("activeTransports &=") == {"connectionRelease"}
    assert sketch.functions_containing("activeTransports =") == set()

    presence = sketch.functions_containing("Serial.isPlugged (") | sketch.functions_containing(
        "Serial.isConnected ("
    )
    assert presence == {"autonomousUsbRendezvous", "printUsbPresence"}

    for name in presence:
        acted_on = sketch.function(name).without_serial_output()
        assert not acted_on.contains("isPlugged ("), f"{name}() acts on isPlugged()"
        assert not acted_on.contains("isConnected ("), f"{name}() acts on isConnected()"


def test_every_way_out_of_a_host_session_releases_usb():
    """RELEASE and lease expiry release USB whether or not autonomous mode is
    armed. LOGGER AUTONOMOUS OFF releases it only when no session is held,
    because a held session is kept (D-031)."""

    sketch = firmware_source.load()
    released = "connectionRelease(CONNECTION_USB);"

    assert sketch.callers("connectionRelease") == {
        "hostSessionRelease",
        "serviceHostLease",
        "stopAutonomousTest",
    }

    release = sketch.function("hostSessionRelease")
    assert release.index("hostSessionHeld = false;") < release.index(released)
    assert release.index(released) < release.index("if (!autonomousTestArmed)")

    expiry = sketch.function("serviceHostLease")
    assert expiry.index("hostSessionHeld = false;") < expiry.index(released)
    assert expiry.index(released) < expiry.index("if (autonomousTestArmed)")
    assert expiry.contains(
        'Serial.println("[CONNECTION] USB transport RELEASED due to lease expiry.");'
    )

    stop = sketch.function("stopAutonomousTest")
    (held,) = stop.blocks_after("if (hostSessionHeld)")
    assert not stop.slice(*held).contains("connectionRelease (")
    assert stop.texts[held[1] + 1] == "else"
    not_held = stop.block_span(held[1] + 2)
    assert stop.slice(*not_held).contains(released)


def test_sleep_policy_asks_whether_any_host_is_connected():
    """The places that decide whether to stay awake ask anyHostConnected(), so a
    later Wi-Fi or BLE lease holds the board awake with no change to them."""

    sketch = firmware_source.load()

    assert sketch.callers("anyHostConnected") == {
        "autonomousUsbRendezvous",
        "autonomousColdBootMaintenanceWindow",
        "setup",
        "printHostSessionStatus",
        "connectionRelease",
    }
    assert sketch.function("autonomousColdBootMaintenanceWindow").contains(
        "if (anyHostConnected())"
    )
    assert sketch.function("setup").contains("if (autonomousTestArmed && anyHostConnected())")


def test_presence_and_claim_are_reported_as_separate_facts():
    """`USB plugged: YES, CDC connected: YES, Active transports: NONE,
    Host claimed: NO` is a valid state, observed on hardware. The rendezvous
    and SESSION STATUS print both halves, in this wording."""

    sketch = firmware_source.load()

    rendezvous = sketch.function("autonomousUsbRendezvous")
    order = [
        rendezvous.index('Serial.print("[AUTO] USB plugged: ");'),
        rendezvous.index('Serial.print("[AUTO] CDC connected: ");'),
        rendezvous.index(
            "printActiveTransports(); "
            'Serial.print("[CONNECTION] Host claimed: "); '
            'Serial.println(anyHostConnected() ? "YES" : "NO");'
        ),
    ]
    assert order == sorted(order)

    assert sketch.function("printHostSessionStatus").contains(
        "printActiveTransports(); "
        'Serial.print("[CONNECTION] Any host connected: "); '
        'Serial.println(anyHostConnected() ? "YES" : "NO");'
    )
