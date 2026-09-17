"""Host session lease semantics: SessionClient and the send/wait rules.

SessionClient is the half of the protocol `app/solar_logger.py` uses. It owns
no port: it writes through a callback and is fed received lines. These tests
drive it exactly that way.

The rule under test throughout is D-030: an echo proves a command was PARSED,
an outcome proves it SUCCEEDED OR FAILED, and session state is keyed on the
outcome. A refused HOLD is not an error - it is how a board that is not in
autonomous mode says no lease is needed.
"""

from __future__ import annotations

import pytest

import device_session as ds
import device_tool as dt


def make_client():
    sent = []
    client = ds.SessionClient(write_line=sent.append, log=lambda _msg: None)
    return client, sent


# ============================================================================
# HOLD
# ============================================================================


def test_hold_is_not_held_until_the_outcome_arrives():
    client, sent = make_client()

    client.request_hold()
    assert sent == [ds.CMD_SESSION_HOLD]
    assert client.held is False, "nothing has come back yet"

    client.note_line("CMD_ACK,LOGGER SESSION HOLD")
    assert client.held is False, "an ACK proves parsing, not a granted lease"

    client.note_line("CMD_RESULT,LOGGER SESSION HOLD,OK")
    assert client.held is True


def test_a_refused_hold_never_becomes_a_session():
    """The firmware echoes a HOLD it is about to refuse (D-030)."""

    client, _sent = make_client()
    client.request_hold()

    client.note_line("CMD_ACK,LOGGER SESSION HOLD")
    client.note_line("CMD_RESULT,LOGGER SESSION HOLD,REFUSED")

    assert client.held is False
    assert client.autonomous_armed is False, (
        "a refusal is how a non-autonomous board announces itself"
    )


def test_hold_refused_is_not_an_error_state():
    # The logger continues in ordinary tethered operation after a refusal, so
    # nothing here should look like a fault.
    client, _sent = make_client()
    client.request_hold()
    client.note_line("CMD_RESULT,LOGGER SESSION HOLD,REFUSED")

    assert client.due_for_keepalive() is False, "no lease means no keepalives"


# ============================================================================
# KEEPALIVE
# ============================================================================


def test_keepalive_counts_only_on_a_successful_outcome():
    client, sent = make_client()
    client.request_hold()
    client.note_line("CMD_ACK,LOGGER SESSION HOLD")
    client.note_line("CMD_RESULT,LOGGER SESSION HOLD,OK")

    client.request_keepalive()
    assert sent[-1] == ds.CMD_SESSION_KEEPALIVE
    assert client.keepalives_sent == 1
    assert client.keepalives_acked == 0

    client.note_line("CMD_ACK,LOGGER SESSION KEEPALIVE")
    client.note_line("CMD_RESULT,LOGGER SESSION KEEPALIVE,OK")
    assert client.keepalives_acked == 1


def test_a_failed_keepalive_drops_the_session():
    client, _sent = make_client()
    client.request_hold()
    client.note_line("CMD_RESULT,LOGGER SESSION HOLD,OK")
    assert client.held is True

    client.request_keepalive()
    client.note_line(ds.RESULT_KEEPALIVE_FAILED)

    assert client.held is False, (
        "the firmware says no session is held, so this client does not hold one"
    )


def test_a_dropped_ack_does_not_discard_the_outcome_that_follows():
    """D-036: the ACK can be lost while the RESULT still arrives.

    With Serial.setTxTimeoutMs(0) a firmware write returns short when the TX
    ring is full, and Arduino's Print does not expose that, so any single
    diagnostic or protocol line can go missing. Requiring the ACK before
    accepting the RESULT meant a granted lease could be dropped on the floor,
    and the next KEEPALIVE would then be refused by a board this client
    believed it had never claimed.
    """

    client, _sent = make_client()
    client.request_hold()

    # No CMD_ACK line at all - it was dropped in transit.
    client.note_line("CMD_RESULT,LOGGER SESSION HOLD,OK")

    assert client.held is True
    assert client.check_pending_timeout() is None, (
        "the command is resolved, so it must not also be reported unacknowledged"
    )


def test_an_unacknowledged_command_is_surfaced_once():
    client, _sent = make_client()
    client.ack_timeout = 0.0

    client.request_hold()

    assert client.check_pending_timeout() == ds.CMD_SESSION_HOLD
    assert client.held is False
    assert client.check_pending_timeout() is None, "reported once, then cleared"


def test_lease_expiry_from_the_firmware_ends_the_session():
    client, _sent = make_client()
    client.request_hold()
    client.note_line("CMD_RESULT,LOGGER SESSION HOLD,OK")

    client.note_line("[SESSION] Host lease EXPIRED after 15021 ms without keepalive.")

    assert client.held is False


# ============================================================================
# RELEASE
# ============================================================================


def test_release_clears_the_session_on_a_successful_outcome():
    client, sent = make_client()
    client.request_hold()
    client.note_line("CMD_RESULT,LOGGER SESSION HOLD,OK")

    client.request_release()
    assert sent[-1] == ds.CMD_SESSION_RELEASE

    client.note_line("CMD_ACK,LOGGER SESSION RELEASE")
    assert client.held is True, "still held until the firmware answers"

    client.note_line("CMD_RESULT,LOGGER SESSION RELEASE,OK")
    assert client.held is False


def test_forget_session_never_claims_the_firmware_released_anything():
    client, _sent = make_client()
    client.request_hold()
    client.note_line("CMD_RESULT,LOGGER SESSION HOLD,OK")

    client.forget_session("serial connection closed")

    # Local state only. The firmware lease expires on its own; that is the
    # recovery path and RELEASE is only an optimization.
    assert client.held is False


# ============================================================================
# Waiting rules (D-040)
# ============================================================================


@pytest.mark.parametrize("requested", [None, True, False])
def test_ordinary_commands_resolve_to_the_requested_wait(requested):
    resolved = dt.resolve_wait("LOGGER STORAGE INFO", requested)

    # None means "nothing asked for", and waiting is the default.
    assert resolved is (True if requested is None else requested)


@pytest.mark.parametrize(
    "command", [ds.CMD_SESSION_KEEPALIVE, ds.CMD_SESSION_RELEASE]
)
def test_session_scoped_commands_never_wait_by_default(command, capsys):
    assert dt.resolve_wait(command, None) is False
    assert dt.resolve_wait(command, False) is False


@pytest.mark.parametrize(
    "command", [ds.CMD_SESSION_KEEPALIVE, ds.CMD_SESSION_RELEASE]
)
def test_explicit_wait_on_a_session_command_is_refused_not_ignored(command, capsys):
    """Silently ignoring a flag is the deviation failure, not the safe option."""

    assert dt.resolve_wait(command, True) is None

    explanation = capsys.readouterr().err
    assert "refused" in explanation.lower()


def test_hold_may_wait_because_it_creates_a_session():
    assert dt.resolve_wait(ds.CMD_SESSION_HOLD, None) is True
    assert dt.resolve_wait(ds.CMD_SESSION_HOLD, True) is True


def test_the_firmware_lease_is_longer_than_the_keepalive_cadence():
    # The host mirrors the firmware's lease only to reason about margin; the
    # firmware stays authoritative. Three keepalives must fit inside one lease
    # so two can be lost without dropping the session.
    assert ds.KEEPALIVE_INTERVAL_SECONDS * 3 <= ds.FIRMWARE_LEASE_SECONDS
