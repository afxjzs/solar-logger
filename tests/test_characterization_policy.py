"""Characterization of the state-policy decisions fixed before modularization.

Part of the pre-modularization gate (docs/DECISIONS.md D-043). Source-level,
read as tokens through tests/firmware_source.py, because these decisions live
inside the monolith and cannot run on the host yet. That is also why no policy
helper was extracted to make them testable: extraction is the modularization,
and it has not started.

What is pinned here is the SHAPE that makes each rule hold: a refusal that
comes before anything is changed, and two ways out of a host session that both
go through the one accounting-safe handoff (D-026, D-034) without disarming.
Whether the board then actually behaves is the hardware acceptance sequence in
docs/LAB_NOTES.md; nothing here claims it.
"""

from __future__ import annotations

import pytest

import firmware_source

DISARMING = (
    "stopAutonomousTest (",
    "saveAutonomousTestArmed (",
    "autonomousTestArmed =",
)


# The values a refusal returns. `false` for the bool handlers; the HOLD
# refusals return their own reasons, which the dispatcher maps to REFUSED and
# ERROR.
REFUSAL_VALUES = frozenset({"false", "HOLD_NOT_ARMED", "HOLD_SLEEP_PENDING"})


def strip_leading_refusals(code: firmware_source.Code) -> firmware_source.Code:
    """Remove every leading `if (...) { return <refusal>; }` statement.

    Serial output must already be gone. What is left is the first thing the
    function does that is not a refusal.
    """

    texts = code.texts
    index = 0

    while texts[index : index + 2] == ["if", "("]:
        condition_end = index + 1
        depth = 0

        for position in range(index + 1, len(texts)):
            if texts[position] == "(":
                depth += 1
            elif texts[position] == ")":
                depth -= 1

                if depth == 0:
                    condition_end = position
                    break

        block = texts[condition_end + 1 : condition_end + 6]

        if not (
            block[:2] == ["{", "return"]
            and block[2] in REFUSAL_VALUES
            and block[3:] == [";", "}"]
        ):
            break

        index = condition_end + 6

    return code.slice(index, len(code))


@pytest.mark.parametrize(
    ("handler", "refusal", "refused_with", "why"),
    [
        pytest.param(
            "armAutonomousTest",
            "if (hostSessionHeld)",
            "false",
            "LOGGER AUTONOMOUS ON with a host session held would deep sleep with "
            "the tethered interval open and discard its charge (defect 6)",
            id="autonomous-on-while-host-session-held",
        ),
        pytest.param(
            "stopAllPowerTests",
            "if (autonomousOwnsBoard())",
            "false",
            "POWER TEST STOP mid-wake would reset the autonomous accumulators and "
            "under-count the next record (defect 4)",
            id="power-test-stop-while-autonomous-owns-board",
        ),
        pytest.param(
            "hostSessionHold",
            "if (pendingAutonomousSleep)",
            "HOLD_SLEEP_PENDING",
            "HOLD inside the deferred-sleep grace used to answer OK and then be "
            "slept on when the grace expired (D-044)",
            id="hold-while-autonomous-sleep-pending",
        ),
    ],
)
def test_refusal_happens_before_anything_changes(handler, refusal, refused_with, why):
    """The refusal exists, returns its refusal, and nothing precedes it but refusals.

    That return is what makes the dispatcher answer ERROR - the mapping is
    pinned in test_characterization_protocol.py. Everything up to and including
    this refusal must be refusals only, so a refused command changes no state.
    """

    body = firmware_source.load().function(handler).without_serial_output()

    assert body.block_after(refusal).texts == firmware_source.words(
        f"return {refused_with};"
    ), f"{handler}(): the `{refusal}` branch must only refuse. {why}."

    guard = body.index(refusal)
    remainder = strip_leading_refusals(body)
    consumed = len(body) - len(remainder)

    assert consumed > guard, (
        f"{handler}(): something other than a refusal runs before `{refusal}`. "
        f"First action: {remainder}"
    )


def test_autonomous_ownership_is_exactly_the_three_measurement_states():
    """What `autonomousOwnsBoard()` means decides what POWER TEST STOP refuses.

    The cold-boot maintenance window and a held host session are deliberately
    NOT ownership: the window is the recovery path (reset, then stop inside
    it), and a host session is ordinary tethered operation whose accounting
    must run (D-026).
    """

    sketch = firmware_source.load()
    owns = sketch.function("autonomousOwnsBoard")

    states = {text for text in owns.texts if text.startswith("AUTO_STATE_")}
    assert states == {
        "AUTO_STATE_TIMER_WAKE_MEASUREMENT",
        "AUTO_STATE_USB_RENDEZVOUS",
        "AUTO_STATE_DEEP_SLEEP_PENDING",
    }

    # POWER TEST STOP resumes only the suspensions it created. Asking the
    # broader intervalAccountingSuspended() was the second half of defect 4.
    stop = sketch.function("stopAllPowerTests")
    assert not stop.contains("intervalAccountingSuspended (")
    assert stop.contains(
        "bool accountingWasSuspended = sleepPowerTestRunning || intervalAccountingBlocked;"
    )


def test_release_and_lease_expiry_resume_autonomous_without_disarming():
    """Both ways out of a held session hand back to autonomous; neither disarms.

    RELEASE (D-035): the shared handoff runs with sleepNow=false, then sleep is
    deferred by the bounded grace so CMD_RESULT can be delivered first.

    Lease expiry (D-025): no command, so nothing to acknowledge. The same
    handoff runs with sleepNow=true and speaks no protocol line.
    """

    sketch = firmware_source.load()

    def sleep_now_argument(body: firmware_source.Code) -> str:
        calls = body.call_arguments("beginAutonomousSleepFromHostSession")
        assert len(calls) == 1, "expected exactly one handoff call"
        return calls[0].texts[-1]

    release = sketch.function("hostSessionRelease")
    assert sleep_now_argument(release) == "false"
    handoff = release.index("beginAutonomousSleepFromHostSession (")
    assert handoff < release.index("armPendingAutonomousSleep(AUTO_SLEEP_HOST_RELEASE,")
    assert not release.contains("autonomousDeepSleepAgain ("), (
        "RELEASE must not sleep before its CMD_RESULT is emitted"
    )

    expiry = sketch.function("serviceHostLease")
    assert sleep_now_argument(expiry) == "true"
    assert not expiry.contains("pendingAutonomousSleep")
    assert not expiry.contains("armPendingAutonomousSleep (")

    for protocol_writer in ("emitCommandAck", "emitCommandResult", "writeProtocolLine"):
        assert not expiry.contains(f"{protocol_writer} ("), (
            "lease expiry has no command, so it must not wait on an acknowledgement"
        )

    # The shared handoff closes the open tethered interval before anything can
    # sleep, keeps the board armed, and sleeps directly only when told to.
    shared = sketch.function("beginAutonomousSleepFromHostSession")
    assert shared.index("closeMeasurementInterval()") < shared.index(
        "autonomousDeepSleepAgain ("
    )
    assert shared.contains("autonomousTestRunning = true;")
    assert shared.contains("setAutoState(AUTO_STATE_DEEP_SLEEP_PENDING, reason);")
    assert shared.block_after("if (sleepNow)").texts == firmware_source.words(
        "autonomousDeepSleepAgain(AUTO_SLEEP_HOST_RELEASE);"
    )

    for name, body in (
        ("hostSessionRelease", release),
        ("serviceHostLease", expiry),
        ("beginAutonomousSleepFromHostSession", shared),
    ):
        for action in DISARMING:
            assert not body.contains(action), f"{name}() disarms: `{action}`"

    # The deferred sleep goes through the shared deadline scheduler (D-034),
    # and loop() services the lease and the deferred sleep after commands.
    pending = sketch.function("servicePendingAutonomousSleep")
    assert pending.contains("autonomousDeepSleepAgain(pendingAutonomousSleepPath);")

    loop = sketch.function("loop")
    assert (
        loop.index("handleSerialCommands();")
        < loop.index("serviceHostLease();")
        < loop.index("servicePendingAutonomousSleep();")
    )

    assert sketch.callers("autonomousDeepSleepAgain") == {
        "runAutonomousWakeCycle",
        "beginAutonomousSleepFromHostSession",
        "servicePendingAutonomousSleep",
        "setup",
    }


def test_deferred_autonomous_sleep_is_armed_and_canceled_in_one_place():
    """The pending-sleep lifecycle, made explicit on 2026-09-18 (D-044).

    - Only three functions write `pendingAutonomousSleep`: arm, cancel, and
      the service that enters the sleep when the grace expires.
    - Arming puts the board in DEEP_SLEEP_PENDING, which autonomousOwnsBoard()
      counts as ownership, so tethered accounting, POWER TEST STOP and RESET YES
      all stand down for the grace. RELEASE and LOGGER AUTONOMOUS ON both arm
      through it, each with its own path.
    - HOLD is refused while a sleep is pending (the refusal test above), so no
      lease can be granted and then slept on.
    - An explicit stop cancels the pending sleep BEFORE its NVS write, so a
      failed write can no longer let the grace expire into deep sleep after
      the operator asked for a stop (D-021). Autonomous mode being off cancels
      it too.
    """

    sketch = firmware_source.load()

    assert sketch.functions_containing("pendingAutonomousSleep =") == {
        "armPendingAutonomousSleep",
        "cancelPendingAutonomousSleep",
        "servicePendingAutonomousSleep",
    }

    arm = sketch.function("armPendingAutonomousSleep")
    assert arm.contains("pendingAutonomousSleep = true;")
    assert arm.contains("pendingAutonomousSleepPath = path;")
    assert arm.contains(
        "pendingAutonomousSleepDeadlineMs = millis() + SESSION_RELEASE_ACK_GRACE_MS;"
    )
    assert arm.contains("setAutoState(AUTO_STATE_DEEP_SLEEP_PENDING, reason);")

    assert sketch.callers("armPendingAutonomousSleep") == {
        "hostSessionRelease",
        "armAutonomousTest",
    }
    assert sketch.function("armAutonomousTest").contains(
        "armPendingAutonomousSleep(AUTO_SLEEP_ARM_COMMAND,"
    )

    # Arming resets the accumulators, and the tethered interval clock moves with
    # them, so a canceled arm resumes ordinary accounting from the real reset.
    arming = sketch.function("armAutonomousTest")
    reset = arming.index("if (!resetInaAccumulators())")
    assert reset < arming.index("intervalStartMs = millis();") < arming.index(
        "armPendingAutonomousSleep ("
    )

    cancel = sketch.function("cancelPendingAutonomousSleep")
    assert cancel.block_after("if (!pendingAutonomousSleep)").texts == (
        firmware_source.words("return;")
    )
    assert cancel.contains("pendingAutonomousSleep = false;")
    assert cancel.contains("setAutoState(AUTO_STATE_IDLE, reason);")
    assert sketch.callers("cancelPendingAutonomousSleep") == {
        "stopAutonomousTest",
        "servicePendingAutonomousSleep",
    }

    stop = sketch.function("stopAutonomousTest")
    assert stop.index("cancelPendingAutonomousSleep (") < stop.index(
        "saveAutonomousTestArmed ("
    )

    service = sketch.function("servicePendingAutonomousSleep").without_serial_output()
    assert service.block_after("if (!autonomousTestArmed)").texts == (
        firmware_source.words(
            'cancelPendingAutonomousSleep("autonomous mode is OFF"); return;'
        )
    )
    assert service.index("pendingAutonomousSleep = false;") < service.index(
        "autonomousDeepSleepAgain(pendingAutonomousSleepPath);"
    )
