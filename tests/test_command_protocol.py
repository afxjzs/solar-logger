"""The machine command protocol: CMD_ACK, CMD_RESULT, and what each proves.

These test real shipped code in `app/device_session.py`. The contract under
test is D-036 as corrected on 2026-09-17:

    CMD_ACK,<command>            parsed and accepted for execution
    CMD_RESULT,<command>,OK      the command actually COMPLETED
    CMD_RESULT,<command>,ERROR   parsed, but did not complete successfully

The defect that motivated the suite: the firmware used to answer OK whenever
the dispatcher merely recognized a command, so a LOGGER AUTONOMOUS OFF whose
NVS write failed reported success and `tools/send.sh` exited 0. The host half
of that contract is what these tests pin.
"""

from __future__ import annotations

import pytest

import device_session as ds

from fake_serial import FakeSerial, FakeSerialError, attach


def make_device(lines):
    device = ds.SerialDevice("/dev/null", log=lambda _msg: None)
    return attach(device, FakeSerial(lines))


# ============================================================================
# Wire strings
# ============================================================================
#
# D-029 requires these to exist in exactly one place. Pinning them against
# literals is what makes a drift between firmware and host show up here rather
# than on hardware.


def test_ack_line_is_the_exact_firmware_text():
    assert ds.ack_line_for("STATUS") == "CMD_ACK,STATUS"


def test_result_prefix_is_the_exact_firmware_text():
    assert ds.result_prefix_for("STATUS") == "CMD_RESULT,STATUS,"


def test_command_text_is_uppercased_to_match_the_firmware():
    # processCommand() uppercases before it echoes, so the host has to expect
    # the uppercased form or it will never match its own command back.
    assert ds.ack_line_for("logger session hold") == "CMD_ACK,LOGGER SESSION HOLD"
    assert (
        ds.result_prefix_for(" logger session hold ")
        == "CMD_RESULT,LOGGER SESSION HOLD,"
    )


# ============================================================================
# ACK and RESULT
# ============================================================================


def test_ack_then_ok_is_a_complete_success():
    device = make_device(
        [
            "CMD_ACK,STATUS",
            "[STATUS] Experiment ID      = 3",
            "CMD_RESULT,STATUS,OK",
        ]
    )

    result = device.send_command("STATUS", ack_timeout=1.0, idle_seconds=0.01)

    assert result.acknowledged is True
    assert result.result_seen is True
    assert result.completed is True
    assert result.result_line == "CMD_RESULT,STATUS,OK"
    assert result.truncated is False


def test_ack_then_error_is_parsed_but_not_successful():
    """The whole point of the stint, from the host side."""

    device = make_device(
        [
            "CMD_ACK,LOGGER AUTONOMOUS OFF",
            "[AUTO] ERROR: Could not clear the persisted flag.",
            "CMD_RESULT,LOGGER AUTONOMOUS OFF,ERROR",
        ]
    )

    result = device.send_command(
        "LOGGER AUTONOMOUS OFF", ack_timeout=1.0, idle_seconds=0.01
    )

    assert result.acknowledged is True, "the firmware did parse it"
    assert result.result_seen is True, "the firmware did answer"
    assert result.completed is False, "but it did not succeed"
    assert result.result_line == "CMD_RESULT,LOGGER AUTONOMOUS OFF,ERROR"


def test_error_result_is_not_reported_as_truncated_output():
    # A failed command reported its outcome, so the capture is complete. Calling
    # it truncated would point the reader at a reporting limit instead of at the
    # failure.
    device = make_device(
        [
            "CMD_ACK,RESET YES",
            "CMD_RESULT,RESET YES,ERROR",
        ]
    )

    result = device.send_command("RESET YES", ack_timeout=1.0, idle_seconds=0.01)

    assert result.completed is False
    assert result.truncated is False
    assert result.stop_reason == "quiet after result"


def test_ack_without_result_is_not_success():
    """A command that was parsed and then went silent must not read as OK."""

    device = make_device(["CMD_ACK,LOGGER AUTONOMOUS ON"])

    result = device.send_command(
        "LOGGER AUTONOMOUS ON",
        ack_timeout=1.0,
        capture_seconds=0.2,
        idle_seconds=0.01,
    )

    assert result.acknowledged is True
    assert result.result_seen is False
    assert result.completed is False
    assert result.truncated is True, "no outcome arrived before the cap"


def test_missing_ack_is_a_failure_and_keeps_the_output_it_saw():
    device = make_device(
        [
            "[BOOT] Wake reason: DEEP SLEEP TIMER",
            "[AUTO] Rendezvous: 1 / 10 s",
        ]
    )

    result = device.send_command("STATUS", ack_timeout=0.2, idle_seconds=0.01)

    assert result.acknowledged is False
    assert result.completed is False
    assert result.stop_reason == "no acknowledgement"
    # The firmware output seen while waiting is kept, because "no ack" plus a
    # blank screen is not diagnosable.
    assert "[BOOT] Wake reason: DEEP SLEEP TIMER" in result.lines


def test_a_result_for_a_different_command_is_not_our_result():
    """Malformed or mismatched framing must not be mistaken for an answer."""

    device = make_device(
        [
            "CMD_ACK,STATUS",
            "CMD_RESULT,VERSION,OK",
            "CMD_RESULT,STATUS",
            "CMD_RESULTSTATUS,OK",
        ]
    )

    result = device.send_command(
        "STATUS", ack_timeout=1.0, capture_seconds=0.2, idle_seconds=0.01
    )

    assert result.acknowledged is True
    assert result.result_seen is False, "none of those framed a STATUS outcome"
    assert result.completed is False


def test_an_unknown_status_word_is_not_success():
    device = make_device(
        [
            "CMD_ACK,STATUS",
            "CMD_RESULT,STATUS,REFUSED",
        ]
    )

    result = device.send_command("STATUS", ack_timeout=1.0, idle_seconds=0.01)

    assert result.result_seen is True
    assert result.completed is False, "only OK means completed"


# ============================================================================
# RELEASE and the expected disconnect
# ============================================================================


def test_release_counts_as_success_only_after_result_ok():
    """RELEASE destroys its own transport, which is expected and not a failure.

    D-035: the firmware emits its result, then waits a bounded grace before
    deep sleep. Losing the port AFTER the result is normal.
    """

    device = make_device(
        [
            "CMD_ACK,LOGGER SESSION RELEASE",
            "[SESSION] Released: ALL OK",
            "CMD_RESULT,LOGGER SESSION RELEASE,OK",
            FakeSerialError("device disappeared"),
        ]
    )

    result = device.send_command(
        "LOGGER SESSION RELEASE", ack_timeout=1.0, idle_seconds=0.01
    )

    assert result.completed is True
    assert result.transport_lost_after_result is True
    assert "transport lost after result" in result.stop_reason


def test_transport_lost_before_any_result_is_a_real_error():
    """The same disconnect, one line earlier, is not a success."""

    device = make_device(
        [
            "CMD_ACK,LOGGER SESSION RELEASE",
            FakeSerialError("device disappeared"),
        ]
    )

    with pytest.raises(ds.DeviceError):
        device.send_command(
            "LOGGER SESSION RELEASE", ack_timeout=1.0, idle_seconds=0.01
        )


def test_release_reporting_not_held_is_not_success():
    # The firmware answers NOT_HELD when no session exists. Harmless, and still
    # not a completed RELEASE.
    device = make_device(
        [
            "CMD_ACK,LOGGER SESSION RELEASE",
            "CMD_RESULT,LOGGER SESSION RELEASE,NOT_HELD",
        ]
    )

    result = device.send_command(
        "LOGGER SESSION RELEASE", ack_timeout=1.0, idle_seconds=0.01
    )

    assert result.result_seen is True
    assert result.completed is False


# ============================================================================
# Which commands may wait for a rendezvous (D-040)
# ============================================================================


@pytest.mark.parametrize(
    "command",
    [
        "LOGGER SESSION KEEPALIVE",
        "LOGGER SESSION RELEASE",
        "logger session release",
        "  LOGGER SESSION KEEPALIVE  ",
    ],
)
def test_session_scoped_commands_require_a_live_session(command):
    assert ds.requires_live_session(command) is True


@pytest.mark.parametrize(
    "command",
    [
        "LOGGER SESSION HOLD",
        "LOGGER SESSION STATUS",
        "STATUS",
        "VERSION",
        "LOGGER AUTONOMOUS ON",
        "LOGGER AUTONOMOUS OFF",
        "LOGGER STORAGE INFO",
        "LOGGER STORAGE DUMP",
    ],
)
def test_other_commands_may_wait_for_the_next_rendezvous(command):
    # HOLD is the one that matters: it CREATES a session rather than addressing
    # one, so any rendezvous will do.
    assert ds.requires_live_session(command) is False


def test_storage_dump_gets_the_longer_capture_window():
    # It streams one line per stored record, and the log already holds over a
    # thousand.
    assert ds.capture_seconds_for("LOGGER STORAGE DUMP") > ds.capture_seconds_for(
        "STATUS"
    )
    assert (
        ds.capture_seconds_for("logger storage dump")
        == ds.RESPONSE_CAPTURE_SECONDS_DUMP
    )
