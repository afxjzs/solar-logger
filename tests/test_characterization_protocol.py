"""Characterization of the machine command contract and the public surface.

Part of the pre-modularization gate (docs/DECISIONS.md D-043). These tests
freeze what the monolith does TODAY, so an extraction stage that changes it
fails here before it reaches hardware.

Two halves, deliberately:

  HOST     `tools/send.sh` end to end, through the real `app/device_tool.py`
           and `app/device_session.py`, against a scripted port. The assertion
           is the exit status a retry loop reads, plus the line an operator
           reads, because those two must never disagree.

  FIRMWARE the dispatcher in the real sketch source, read as tokens by
           tests/firmware_source.py. What each command reports, and that every
           command is acknowledged once and answered once.

The host-side protocol unit tests in test_command_protocol.py and
test_session_semantics.py stay as they are; nothing here repeats them.
"""

from __future__ import annotations

from typing import ClassVar

import pytest

import device_session as ds
import device_tool as dt
import firmware_source
from conftest import PROJECT_ROOT
from fake_serial import FakeSerial, FakeSerialError, attach

# ============================================================================
# HOST: what tools/send.sh reports for each wire transcript
# ============================================================================


class ScriptedDevice(ds.SerialDevice):
    """A SerialDevice whose open() attaches a scripted port instead of hardware."""

    transcript: ClassVar[list] = []

    def open(self) -> None:
        attach(self, FakeSerial(self.transcript))


@pytest.fixture
def send(monkeypatch, tmp_path, capsys):
    """Run `device_tool.py send` exactly as tools/send.sh invokes it."""

    # Never queue to a real logger or honor a real upload marker: either would
    # make the result depend on what else is running on this machine, and the
    # logger path WRITES .serial-command into the repository.
    monkeypatch.setattr(dt, "logger_owns_serial", lambda: False)
    monkeypatch.setattr(dt, "UPLOAD_HANDSHAKE_FILE", tmp_path / "no-upload")
    monkeypatch.setattr(ds, "find_port", lambda: "/dev/cu.usbmodem-scripted")
    monkeypatch.setattr(ds, "SerialDevice", ScriptedDevice)

    # cmd_send reads the ACK timeout from the module at call time. Shortened so
    # the no-ACK cases do not each spin for the production four seconds.
    monkeypatch.setattr(ds, "ACK_TIMEOUT_SECONDS", 0.2)

    def run(command: str, transcript: list):
        monkeypatch.setattr(ScriptedDevice, "transcript", transcript)
        status = dt.main(
            [
                "send",
                "--capture-seconds",
                "0.3",
                "--idle-seconds",
                "0.01",
                "--",
                *command.split(),
            ]
        )
        return status, capsys.readouterr()

    return run


COMPLETED_LINE = "Firmware completed command: ALL OK"
COMPLETED_WITHOUT_ACK_LINE = "Firmware completed command, but CMD_ACK was NOT observed"

# How send.sh must report the outcome, alongside its exit status.
CLEAN = "clean"  # exit 0, "completed: ALL OK"
WITHOUT_ACK = "without-ack"  # exit 0, a distinct line and a warning (D-045)
FAILED = "failed"  # exit 1, "NOT ALL OK", never a completion line

# (command, what the firmware sent, how send.sh must report it)
TRANSCRIPTS = [
    pytest.param(
        "STATUS",
        ["CMD_ACK,STATUS", "[STATUS] Experiment ID      = 3", "CMD_RESULT,STATUS,OK"],
        CLEAN,
        id="ack-then-ok-is-success",
    ),
    pytest.param(
        "LOGGER AUTONOMOUS OFF",
        [
            "CMD_ACK,LOGGER AUTONOMOUS OFF",
            "[AUTO] ERROR: Could not clear the persisted flag.",
            "CMD_RESULT,LOGGER AUTONOMOUS OFF,ERROR",
        ],
        FAILED,
        id="handler-failed-is-failure",
    ),
    pytest.param(
        "LOGGER AUTONOMOUS ON",
        ["CMD_ACK,LOGGER AUTONOMOUS ON"],
        FAILED,
        id="ack-without-result-is-failure",
    ),
    pytest.param(
        "STATUS",
        ["CMD_RESULT,STATUS,OK"],
        WITHOUT_ACK,
        id="result-ok-without-ack-is-completed-and-flagged",
    ),
    pytest.param(
        "RESET YES",
        ["CMD_RESULT,RESET YES,ERROR"],
        FAILED,
        id="result-error-without-ack-is-failure",
    ),
    pytest.param(
        "STATUS",
        ["CMD_RESULT,STATUS,ERROR", "CMD_ACK,STATUS", "CMD_RESULT,STATUS,OK"],
        CLEAN,
        id="stale-result-before-ack-is-discarded",
    ),
    pytest.param(
        "STATUS",
        ["CMD_RESULT,STATUS,OK", "CMD_ACK,STATUS", "CMD_RESULT,STATUS,ERROR"],
        FAILED,
        id="stale-ok-before-ack-does-not-become-success",
    ),
    pytest.param(
        "FROB",
        [
            "CMD_ACK,FROB",
            "[WARNING] Unknown command: FROB",
            "CMD_RESULT,FROB,ERROR",
        ],
        FAILED,
        id="unknown-command-is-failure",
    ),
    pytest.param(
        "STATUS",
        ["CMD_ACK,STATUS", "CMD_RESULT,VERSION,OK", "CMD_RESULTSTATUS,OK"],
        FAILED,
        id="someone-elses-result-is-not-ours",
    ),
    pytest.param(
        "LOGGER SESSION HOLD",
        ["CMD_ACK,LOGGER SESSION HOLD", "CMD_RESULT,LOGGER SESSION HOLD,REFUSED"],
        FAILED,
        id="hold-refused-is-failure",
    ),
    pytest.param(
        "LOGGER SESSION HOLD",
        [
            "CMD_ACK,LOGGER SESSION HOLD",
            "[SESSION] NOT ALL OK - HOLD not granted: autonomous sleep is pending.",
            "CMD_RESULT,LOGGER SESSION HOLD,ERROR",
        ],
        FAILED,
        id="hold-during-pending-sleep-is-failure",
    ),
    pytest.param(
        "LOGGER SESSION KEEPALIVE",
        [
            "CMD_ACK,LOGGER SESSION KEEPALIVE",
            "CMD_RESULT,LOGGER SESSION KEEPALIVE,FAILED",
        ],
        FAILED,
        id="keepalive-failed-is-failure",
    ),
    pytest.param(
        "LOGGER SESSION RELEASE",
        [
            "CMD_ACK,LOGGER SESSION RELEASE",
            "[SESSION] Released: ALL OK",
            "CMD_RESULT,LOGGER SESSION RELEASE,OK",
            FakeSerialError("device disappeared"),
        ],
        CLEAN,
        id="release-disconnect-after-ok-is-success",
    ),
    pytest.param(
        "LOGGER SESSION RELEASE",
        [
            "CMD_ACK,LOGGER SESSION RELEASE",
            "[SESSION] NOT ALL OK - RELEASE did not complete. Failed stage: "
            "closing the open tethered interval",
            "CMD_RESULT,LOGGER SESSION RELEASE,ERROR",
        ],
        FAILED,
        id="release-handoff-failed-is-failure",
    ),
    pytest.param(
        "LOGGER SESSION RELEASE",
        ["CMD_ACK,LOGGER SESSION RELEASE", FakeSerialError("device disappeared")],
        FAILED,
        id="release-disconnect-before-result-is-failure",
    ),
    pytest.param(
        "LOGGER SESSION RELEASE",
        [
            "CMD_ACK,LOGGER SESSION RELEASE",
            "[SESSION] Released: ALL OK",
            FakeSerialError("device disappeared"),
        ],
        FAILED,
        id="release-human-line-then-disconnect-is-not-success",
    ),
    pytest.param(
        "LOGGER SESSION RELEASE",
        ["CMD_ACK,LOGGER SESSION RELEASE", "CMD_RESULT,LOGGER SESSION RELEASE,NOT_HELD"],
        FAILED,
        id="release-not-held-is-failure",
    ),
]


@pytest.mark.parametrize(("command", "transcript", "report"), TRANSCRIPTS)
def test_send_exit_status_follows_the_command_contract(
    send, command, transcript, report
):
    """Only CMD_RESULT,...,OK proves completion (D-041), and how it is reported
    depends on whether its CMD_ACK was seen (D-045).

    The report is checked alongside the exit status because a status line that
    contradicts the exit code is a silent failure in its own right.
    """

    status, output = send(command, transcript)

    assert status == (1 if report == FAILED else 0), output.out + output.err

    if report == CLEAN:
        assert COMPLETED_LINE in output.out
        assert "CMD_ACK was NOT observed" not in output.err

    elif report == WITHOUT_ACK:
        # Completed, and never passed off as a clean exchange: no ALL OK
        # completion line, a distinct one instead, a warning, and the RESULT
        # line itself visible.
        assert COMPLETED_LINE not in output.out
        assert COMPLETED_WITHOUT_ACK_LINE in output.out
        assert "CMD_ACK was NOT observed" in output.err
        assert f"CMD_RESULT,{command},OK" in output.out

    else:
        assert COMPLETED_LINE not in output.out, (
            "a failed command must never print the completion line"
        )
        assert COMPLETED_WITHOUT_ACK_LINE not in output.out
        assert "NOT ALL OK" in output.err

    result_came_first = str(transcript[0]).startswith("CMD_RESULT,")
    ack_came_later = f"CMD_ACK,{command}" in transcript[1:]

    if result_came_first and ack_came_later:
        # The ACK arrived after a result, which one FIFO makes impossible for
        # that result's own command: it was stale, and saying so is required.
        assert "discarded a stale result" in output.err


# ============================================================================
# FIRMWARE: the dispatcher, read from the real sketch
# ============================================================================

ALWAYS_OK = "cannot fail; reports OK whenever reached"
OWN_RESULT = "emits its own status word"
INLINE = "decides commandOk inside the branch"

# Every other value is the handler call whose bool the branch assigns to
# commandOk, arguments included, so a swapped variant flag shows up too.

# The commands the host tools, the docs and the hardware procedures use.
CANONICAL_COMMANDS = {
    "VERSION": ALWAYS_OK,
    "STATUS": "printStatus()",
    "LOGGER AUTONOMOUS ON": "armAutonomousTest()",
    "LOGGER AUTONOMOUS OFF": "stopAutonomousTest()",
    "LOGGER AUTONOMOUS STATUS": ALWAYS_OK,
    "LOGGER SESSION HOLD": OWN_RESULT,
    "LOGGER SESSION KEEPALIVE": OWN_RESULT,
    "LOGGER SESSION RELEASE": OWN_RESULT,
    "LOGGER SESSION STATUS": ALWAYS_OK,
    "LOGGER STORAGE INFO": "printStorageInfo()",
    "LOGGER STORAGE DUMP": "dumpStorage()",
    "LOGGER STORAGE CLEAR YES": "clearStorage()",
    "LOGGER INTERVAL *": INLINE,
    "RESET": ALWAYS_OK,
    "RESET YES": "resetExperiment()",
}

# Everything else the dispatcher accepts: bench power tests, Wi-Fi, HELP, the
# confirmation prompt, and the deprecated LOGGER TEST aliases (D-031).
OTHER_COMMANDS = {
    "HELP": ALWAYS_OK,
    "WIFI ON": "enableWifi()",
    "WIFI OFF": "disableWifi()",
    "WIFI STATUS": ALWAYS_OK,
    "POWER TEST WIFI": "armWifiPowerTest()",
    "POWER TEST SLEEP": "armSleepPowerTest(false)",
    "POWER TEST SLEEP INA OFF": "armSleepPowerTest(true)",
    "POWER TEST STOP": "stopAllPowerTests()",
    "POWER TEST STATUS": ALWAYS_OK,
    "LOGGER TEST AUTONOMOUS": "armAutonomousTest()",
    "LOGGER TEST STATUS": ALWAYS_OK,
    "LOGGER TEST STOP": "stopAutonomousTest()",
    "LOGGER STORAGE CLEAR": ALWAYS_OK,
}

COMMAND_SURFACE = {**CANONICAL_COMMANDS, **OTHER_COMMANDS}


def dispatch_branches() -> dict[str, firmware_source.Code]:
    """Map each command processCommand() accepts to the body of its branch.

    `command == "X"` is an exact match. `command.startsWith("X")` is recorded
    as "X*" with the trailing space kept, so "LOGGER INTERVAL *" means the
    prefix "LOGGER INTERVAL ".
    """

    body = firmware_source.load().function("processCommand")
    branches: dict[str, firmware_source.Code] = {}

    for index, text in enumerate(body.texts):
        if text != "if" or body.texts[index + 1] != "(":
            continue

        start = index + 2
        close = start + body.texts[start:].index(")")
        condition = body.texts[start:close]

        if len(condition) == 3 and condition[:2] == ["command", "=="]:
            key = firmware_source.unquote(condition[2])
        elif condition[:4] == ["command", ".", "startsWith", "("]:
            close = body.texts.index(")", close + 1)
            key = firmware_source.unquote(condition[4]) + "*"
        else:
            continue

        first, last = body.block_span(close + 1)
        assert key not in branches, f"{key!r} is dispatched twice"
        branches[key] = body.slice(first + 1, last)

    return branches


def result_words(code: firmware_source.Code) -> set[str]:
    """Status words passed to emitCommandResult() - not the Serial messages."""

    return {
        firmware_source.unquote(text)
        for arguments in code.call_arguments("emitCommandResult")
        for text in arguments.strings()
    }


def test_command_surface_is_frozen():
    """The exact command set, and HELP still lists every canonical command.

    Adding or removing a command is a behavior change (a MINOR version bump
    under the PROJECT.md policy), so it has to be made here on purpose rather
    than discovered on hardware after a file move.
    """

    assert set(dispatch_branches()) == set(COMMAND_SURFACE)

    help_text = " ".join(
        firmware_source.unquote(text)
        for text in firmware_source.load().function("printHelp").strings()
    )

    for command in CANONICAL_COMMANDS:
        shown = command.replace("*", "<seconds>")
        assert shown in help_text, f"HELP no longer lists {shown}"


def test_every_command_is_acked_once_and_answered_once_by_its_real_outcome():
    """D-041 in the firmware: ACK means parsed, RESULT means completed.

    - An empty line gets neither.
    - Every other line gets exactly one CMD_ACK, before dispatch, so a command
      about to be refused is still acknowledged (D-030).
    - Every path emits exactly one CMD_RESULT: the three session branches emit
      their own and set `resultEmitted`; every other branch reaches the single
      emission site, which reports `recognized && commandOk`.
    - A fallible handler's bool is what reaches that site, so a refusal or a
      failed write answers ERROR, never OK.
    - An unknown command answers ERROR.
    """

    body = firmware_source.load().function("processCommand")

    empty_return = body.index("if (command.length() == 0) { return; }")
    ack = body.index("emitCommandAck(command)")
    first_branch = body.index('if (command == "HELP")')

    assert body.count("emitCommandAck (") == 1
    assert empty_return < ack < first_branch

    assert body.count("emitCommandResult (") == 4, "three session branches + one"

    final = body.block_after("if (!resultEmitted)")
    assert final.contains("const bool succeeded = recognized && commandOk;")
    assert final.contains('emitCommandResult(command, succeeded ? "OK" : "ERROR")')
    assert body.contains("else { recognized = false;"), "unknown command"

    branches = dispatch_branches()

    for command, rule in COMMAND_SURFACE.items():
        branch = branches[command]

        if rule == ALWAYS_OK:
            assert not branch.contains("commandOk"), command
            assert not branch.contains("emitCommandResult"), command
        elif rule == OWN_RESULT:
            assert branch.count("emitCommandResult (") == 1, command
            assert branch.find("resultEmitted = true;") > branch.find(
                "emitCommandResult ("
            ), command
        elif rule == INLINE:
            # LOGGER INTERVAL refuses four ways: not a number, out of range,
            # armed, and a failed NVS write. Each must reach the result.
            assert branch.count("commandOk = false;") == 4, command
            assert not branch.contains("emitCommandResult"), command
        else:
            assert branch.contains(f"commandOk = {rule};"), (
                f"{command}: the result of {rule} no longer reaches CMD_RESULT"
            )
            assert not branch.contains("emitCommandResult"), command

    # No other code path may speak the machine protocol.
    sketch = firmware_source.load()
    assert sketch.callers("emitCommandAck") == {"processCommand"}
    assert sketch.callers("emitCommandResult") == {"processCommand"}
    assert sketch.callers("writeProtocolLine") == {
        "emitCommandAck",
        "emitCommandResult",
    }


def test_session_commands_report_their_own_status_words():
    """The three session commands keep their more specific words (D-041).

    The host treats every word except OK as not completed; the host half is in
    test_send_exit_status_follows_the_command_contract.
    """

    branches = dispatch_branches()

    # HOLD: REFUSED means autonomous is off and no lease is needed; ERROR means
    # the board is already committed to a deferred sleep (D-044).
    assert branches["LOGGER SESSION HOLD"].contains("const HoldResult hold = hostSessionHold();")
    assert branches["LOGGER SESSION HOLD"].contains(
        'emitCommandResult(command, hold == HOLD_GRANTED ? "OK" '
        ': (hold == HOLD_NOT_ARMED ? "REFUSED" : "ERROR"))'
    )
    assert branches["LOGGER SESSION KEEPALIVE"].contains(
        'emitCommandResult(command, hostKeepaliveCount > keepalivesBefore ? "OK" : "FAILED")'
    )
    assert result_words(branches["LOGGER SESSION RELEASE"]) == {"OK", "ERROR", "NOT_HELD"}

    # The complete vocabulary. A new word is a protocol change for every host.
    dispatcher = firmware_source.load().function("processCommand")
    assert result_words(dispatcher) == {"OK", "ERROR", "REFUSED", "FAILED", "NOT_HELD"}


def test_release_can_report_a_failed_handoff():
    """RELEASE answers OK only when it COMPLETED (D-041, D-044).

    This was a strict xfail until 2026-09-18: the dispatcher keyed RELEASE's
    result on `wasHeld` alone, so a handoff that failed and left the board
    awake still answered OK and tools/send.sh exited 0.

    Completed now means, and only means:

      a session was held, and it was ended
      and, with autonomous mode armed, the handoff was PREPARED
      and the deferred sleep was ARMED

    Entering the sleep is not part of it: that happens after the result, by
    design (D-035).
    """

    sketch = firmware_source.load()
    branch = dispatch_branches()["LOGGER SESSION RELEASE"]

    # Not held -> NOT_HELD. Held and completed -> OK. Held and failed -> ERROR.
    assert branch.contains("const bool wasHeld = hostSessionHeld;")
    assert branch.contains("const bool released = hostSessionRelease();")
    assert branch.contains(
        'emitCommandResult(command, wasHeld ? (released ? "OK" : "ERROR") : "NOT_HELD")'
    )

    release = sketch.function("hostSessionRelease").without_serial_output()

    # Every exit that is not a completion returns false: the not-held case and
    # a handoff that was not prepared.
    assert release.block_after("if (!hostSessionHeld)").texts == firmware_source.words(
        "if (autonomousTestArmed) { } return false;"
    )
    assert release.block_after("if (handoff != HANDOFF_PREPARED)").texts == (
        firmware_source.words("return false;")
    )

    # The two completions: not armed (the board simply stays awake), and armed
    # with the handoff prepared and the deferred sleep armed - in that order.
    assert release.count("return true;") == 2
    handoff = release.index("beginAutonomousSleepFromHostSession (")
    armed = release.index("armPendingAutonomousSleep(AUTO_SLEEP_HOST_RELEASE,")
    last_success = release.find_all("return true;")[-1]
    assert handoff < armed < last_success

    # Each handoff failure names its stage; success is returned exactly once,
    # after the next autonomous interval's baseline and state are set.
    shared = sketch.function("beginAutonomousSleepFromHostSession")
    assert shared.count("return HANDOFF_INTERVAL_NOT_CLOSED;") == 1
    assert shared.count("return HANDOFF_STORAGE_UNAVAILABLE;") == 1
    assert shared.count("return HANDOFF_PREPARED;") == 1
    assert shared.index("setAutoState(AUTO_STATE_DEEP_SLEEP_PENDING, reason);") < (
        shared.index("return HANDOFF_PREPARED;")
    )


def test_firmware_identity_markers_are_preserved():
    """Version, revision and build ID (D-038), and every place they print.

    The version and build ID are pinned exactly. Bumping either is a
    deliberate act under the PROJECT.md policy; update the expectation here in
    the same commit. A pure file move must change neither.
    """

    sketch = firmware_source.load()

    assert sketch.code.contains('#define FIRMWARE_VERSION "0.3.0-dev"')
    assert sketch.code.contains(
        '#define FIRMWARE_BUILD_ID "solar-logger-protocol-ack-v3"'
    )

    # Revision: empty unless tools/upload.sh injects it, and an empty one is
    # announced as UNKNOWN rather than printed as a blank.
    assert sketch.code.contains(
        '#ifndef FIRMWARE_GIT_REV #define FIRMWARE_GIT_REV "" #endif'
    )

    identity = sketch.function("printFirmwareIdentity")
    assert identity.contains("if (FIRMWARE_GIT_REV[0] == '\\0')")
    assert identity.contains(
        '"UNKNOWN - not injected by this build (use tools/upload.sh)"'
    )

    upload = (PROJECT_ROOT / "tools" / "upload.sh").read_text(encoding="utf-8")
    assert "compiler.cpp.extra_flags=-DFIRMWARE_GIT_REV=" in upload

    # Printed on VERSION, inside STATUS, and at the top of setup() - before the
    # timer-wake branch, so every autonomous wake identifies itself too.
    assert sketch.callers("printFirmwareIdentity") == {
        "processCommand",
        "printStatus",
        "setup",
    }
    assert dispatch_branches()["VERSION"].contains("printFirmwareIdentity();")

    setup = sketch.function("setup")
    assert setup.index("printFirmwareIdentity();") < setup.index(
        "if (bootWakeupCause == ESP_SLEEP_WAKEUP_TIMER)"
    )
