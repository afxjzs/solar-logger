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
from conftest import PROJECT_ROOT
from fake_serial import FakeSerialError


def make_client():
    sent = []
    client = ds.SessionClient(write_line=sent.append, log=lambda _msg: None)
    return client, sent


def make_logging_client():
    """A client whose log lines are kept, for tests about what it SAYS."""

    sent = []
    logged: list[str] = []
    client = ds.SessionClient(write_line=sent.append, log=logged.append)
    return client, sent, logged


def held_client():
    client, sent, logged = make_logging_client()
    client.request_hold()
    client.note_line("CMD_ACK,LOGGER SESSION HOLD")
    client.note_line("CMD_RESULT,LOGGER SESSION HOLD,OK")
    logged.clear()
    return client, sent, logged


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


# ============================================================================
# The machine RESULT is the authority (D-045)
# ============================================================================
#
# The firmware prints its human outcome line BEFORE the machine RESULT. That
# line used to consume the expected outcome, so the RESULT after it was
# ignored and the human line decided everything. These feed the real order.


def test_the_machine_result_decides_after_a_human_outcome_line():
    # RELEASE: the outcome is recorded from the RESULT, not the human line.
    client, _sent, _logged = held_client()
    client.request_release()
    client.note_line("CMD_ACK,LOGGER SESSION RELEASE")
    client.note_line("[SESSION] Released: ALL OK")
    assert client.release_result is None, "the human line decides nothing"

    client.note_line("CMD_RESULT,LOGGER SESSION RELEASE,OK")
    assert client.release_result == "OK"
    assert client.release_ack_observed is True
    assert client.held is False

    # KEEPALIVE: one renewal, counted once, from the RESULT.
    client, _sent, _logged = held_client()
    client.request_keepalive()
    client.note_line("CMD_ACK,LOGGER SESSION KEEPALIVE")
    client.note_line("[SESSION] KEEPALIVE 1: lease renewed for 15000 ms: ALL OK")
    client.note_line("CMD_RESULT,LOGGER SESSION KEEPALIVE,OK")
    assert client.keepalives_acked == 1

    # A failed KEEPALIVE ends the session from the RESULT alone, even when the
    # human line was the one that got dropped.
    client, _sent, _logged = held_client()
    client.request_keepalive()
    client.note_line("CMD_ACK,LOGGER SESSION KEEPALIVE")
    client.note_line("CMD_RESULT,LOGGER SESSION KEEPALIVE,FAILED")
    assert client.held is False


def test_hold_refused_because_sleep_is_pending_is_not_a_session():
    """D-044: HOLD inside the deferred-sleep grace answers ERROR.

    Unlike REFUSED, that does not mean autonomous mode is off: the board is
    armed and about to sleep, and the next rendezvous can be claimed.
    """

    client, _sent, _logged = make_logging_client()
    client.request_hold()
    client.note_line("CMD_ACK,LOGGER SESSION HOLD")
    client.note_line(
        "[SESSION] NOT ALL OK - HOLD not granted: autonomous sleep is pending."
    )
    client.note_line("CMD_RESULT,LOGGER SESSION HOLD,ERROR")

    assert client.held is False
    assert client.autonomous_armed is True
    assert client.due_for_keepalive() is False


def test_a_result_without_its_ack_is_accepted_but_never_silently():
    """D-045: accepted as the outcome, logged and counted as not clean."""

    clean, _sent, clean_log = make_logging_client()
    clean.request_hold()
    clean.note_line("CMD_ACK,LOGGER SESSION HOLD")
    clean.note_line("CMD_RESULT,LOGGER SESSION HOLD,OK")

    lossy, _sent, lossy_log = make_logging_client()
    lossy.request_hold()
    lossy.note_line("CMD_RESULT,LOGGER SESSION HOLD,OK")

    assert clean.held is True and lossy.held is True, "both leases were granted"

    assert clean.results_without_ack == 0
    assert not any("CMD_ACK was NOT observed" in line for line in clean_log)

    assert lossy.results_without_ack == 1
    assert any("CMD_ACK was NOT observed" in line for line in lossy_log), (
        "an outcome whose ACK was lost must not read like a clean exchange"
    )


def test_an_ack_after_an_accepted_result_proves_that_result_stale():
    """One FIFO: a command's ACK always precedes its own RESULT.

    So an ACK arriving after a result was accepted for that command means the
    result belonged to an earlier send of it. Discard it; wait for ours.
    """

    client, _sent, logged = make_logging_client()
    client.request_hold()

    client.note_line("CMD_RESULT,LOGGER SESSION HOLD,REFUSED")
    assert client.held is False

    client.note_line("CMD_ACK,LOGGER SESSION HOLD")
    assert client.stale_results_discarded == 1
    assert any("stale" in line for line in logged)

    client.note_line("CMD_RESULT,LOGGER SESSION HOLD,OK")
    assert client.held is True
    assert client.autonomous_armed is True


# ============================================================================
# Releasing and reporting it (the logger's shutdown path)
# ============================================================================


def scripted_reader(items):
    """A read_line() that replays items: str, None (timeout), or an exception."""

    queue = list(items)

    def read_line():
        if not queue:
            return None

        item = queue.pop(0)

        if isinstance(item, BaseException):
            raise item

        return item

    return read_line


RELEASED_ALL_OK = "Board released: ALL OK"


@pytest.mark.parametrize(
    ("replies", "released", "must_say"),
    [
        pytest.param(
            [
                "CMD_ACK,LOGGER SESSION RELEASE",
                "[SESSION] Released: ALL OK",
                "CMD_RESULT,LOGGER SESSION RELEASE,OK",
            ],
            True,
            RELEASED_ALL_OK,
            id="result-ok-is-released",
        ),
        pytest.param(
            [
                "CMD_ACK,LOGGER SESSION RELEASE",
                "[SESSION] NOT ALL OK - RELEASE did not complete. Failed stage: "
                "closing the open tethered interval",
                "CMD_RESULT,LOGGER SESSION RELEASE,ERROR",
            ],
            False,
            "answered RELEASE with ERROR",
            id="result-error-is-a-failure",
        ),
        pytest.param(
            [
                "CMD_ACK,LOGGER SESSION RELEASE",
                "CMD_RESULT,LOGGER SESSION RELEASE,NOT_HELD",
            ],
            False,
            "answered RELEASE with NOT_HELD",
            id="not-held-is-not-released",
        ),
        pytest.param(
            [
                "[SESSION] Host lease EXPIRED after 15021 ms without keepalive.",
                "CMD_ACK,LOGGER SESSION RELEASE",
                "CMD_RESULT,LOGGER SESSION RELEASE,NOT_HELD",
            ],
            False,
            "answered RELEASE with NOT_HELD",
            id="lease-expiry-line-is-not-a-release",
        ),
        pytest.param(
            ["CMD_ACK,LOGGER SESSION RELEASE", FakeSerialError("device disappeared")],
            False,
            "outcome is UNKNOWN",
            id="disconnect-before-result-is-unknown",
        ),
        pytest.param(
            [
                "CMD_ACK,LOGGER SESSION RELEASE",
                "[SESSION] Released: ALL OK",
                FakeSerialError("device disappeared"),
            ],
            False,
            "outcome is UNKNOWN",
            id="human-line-then-disconnect-is-unknown",
        ),
        pytest.param(
            ["CMD_ACK,LOGGER SESSION RELEASE"],
            False,
            "outcome is UNKNOWN",
            id="no-result-before-timeout-is-unknown",
        ),
        pytest.param(
            ["[SESSION] Released: ALL OK", "CMD_RESULT,LOGGER SESSION RELEASE,OK"],
            True,
            "CMD_ACK was NOT observed",
            id="result-ok-without-ack-is-released-but-flagged",
        ),
    ],
)
def test_release_is_reported_from_the_machine_result_only(replies, released, must_say):
    """Fix 3: "Board released: ALL OK" only for a clean RESULT OK.

    The old shutdown path printed it whenever the session stopped being held,
    which a NOT_HELD answer, a lease-expiry line, or the human release line all
    caused. A vanished port is never evidence of success.
    """

    client, _sent, logged = held_client()
    echoed: list[str] = []

    report = ds.release_session(
        client, scripted_reader(replies), echo=echoed.append, timeout=0.05
    )

    assert report.sent is True
    assert report.released is released
    assert any(must_say in line for line in logged), logged

    clean_ok = released and report.ack_observed
    said_all_ok = any(RELEASED_ALL_OK in line for line in logged)
    assert said_all_ok is clean_ok, logged

    if not released:
        assert any("NOT ALL OK" in line for line in logged)

    # Nothing the firmware said was hidden.
    assert echoed == [item for item in replies if isinstance(item, str)]


def test_a_release_that_cannot_be_written_says_so():
    client, _sent, logged = held_client()

    def broken_write(_command):
        raise OSError("port closed")

    client.write_line = broken_write

    report = ds.release_session(client, scripted_reader([]), timeout=0.05)

    assert report.sent is False
    assert report.released is False
    assert any("could not be sent" in line for line in logged)
    assert not any(RELEASED_ALL_OK in line for line in logged)


def test_the_logger_reports_release_only_through_the_shared_procedure():
    """app/solar_logger.py must not decide RELEASE success on its own again."""

    logger = (PROJECT_ROOT / "app" / "solar_logger.py").read_text(encoding="utf-8")

    assert "ds.release_session(" in logger

    # The message as a string literal, so a comment explaining the history does
    # not count as the logger printing it.
    assert '"[SESSION] Board released' not in logger
