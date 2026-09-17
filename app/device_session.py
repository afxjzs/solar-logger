"""
Shared host-side device and session protocol for the BMW Solar Logger.

This is the ONE place the host implements the sleeping-device and host-session
protocol. Three programs speak it and none of them may reimplement it:

    app/solar_logger.py   long-lived tethered logger, owns Serial itself
    tools/send.sh         one-shot command sender, via app/device_tool.py
    tools/upload.sh       sleep-aware uploader, via app/device_tool.py

WHAT BELONGS HERE

Port discovery, waiting for a sleeping board to reappear at its next autonomous
rendezvous, opening Serial, sending a command, requiring the firmware's exact
acknowledgement, capturing a response, and the SESSION HOLD / KEEPALIVE /
RELEASE lease.

WHAT DOES NOT

Plotting, CSV schemas, experiment accounting, or anything about what the
firmware does internally. Those belong to their applications. This module knows
the wire protocol and nothing else.

TWO WAYS TO USE IT

    SerialDevice   owns a port for one-shot work. It reads the port itself.

    SessionClient  does NOT own a port. It writes commands through a callback
                   and is fed received lines by whoever does own the port.
                   solar_logger.py needs this, because it runs its own read
                   loop and a second reader would fight it.

TRANSPORTS

The firmware decides whether it may sleep by asking anyHostConnected() against a
transport bitmask, never by asking about USB. The host mirrors that: a session
is a lease held over SOME transport, and USB is simply the only one implemented
today. Nothing here should grow a USB-shaped assumption about what a session
means.
"""

from __future__ import annotations

import glob
import time
from dataclasses import dataclass, field
from enum import IntFlag
from typing import Callable, Optional

try:
    import serial

except ImportError as error:  # pragma: no cover - environment problem
    raise SystemExit(
        "[DEVICE] ERROR: pyserial is not available to this interpreter.\n"
        "[DEVICE] Install it, or run through the project environment:\n"
        "[DEVICE]     uv run python ...\n"
        f"[DEVICE] Import error: {error}"
    )


# ============================================================================
# TRANSPORTS
# ============================================================================
#
# Mirrors CONNECTION_USB / CONNECTION_WIFI / CONNECTION_BLE in the firmware.
# Only USB is implemented on either side today.


class Transport(IntFlag):
    NONE = 0
    USB = 1
    WIFI = 2
    BLE = 4


# ============================================================================
# PROTOCOL CONSTANTS
# ============================================================================
#
# Every magic string the host needs lives here. Duplicating any of them into an
# application is how the three callers drift apart.

BAUD_RATE = 115200

PORT_GLOB = "/dev/cu.usbmodem*"

# Machine protocol lines are separate from human-readable diagnostics. The
# human echo may be dropped when HWCDC is under backpressure; these lines are
# written through the firmware's bounded protocol writer instead.
CMD_ACK_PREFIX = "CMD_ACK,"
CMD_RESULT_PREFIX = "CMD_RESULT,"

CMD_SESSION_HOLD = "LOGGER SESSION HOLD"
CMD_SESSION_KEEPALIVE = "LOGGER SESSION KEEPALIVE"
CMD_SESSION_RELEASE = "LOGGER SESSION RELEASE"
CMD_SESSION_STATUS = "LOGGER SESSION STATUS"

# Autonomous mode. Canonical as of 2026-09-11, when it graduated from a bench
# test to the real operating mode. The firmware still accepts the older
# LOGGER TEST * spelling and prints a deprecation notice for it; host code uses
# these names only.
CMD_AUTONOMOUS_ON = "LOGGER AUTONOMOUS ON"
CMD_AUTONOMOUS_OFF = "LOGGER AUTONOMOUS OFF"
CMD_AUTONOMOUS_STATUS = "LOGGER AUTONOMOUS STATUS"

# Human-readable OUTCOME LINES retained for logger display.
#
# Machine CMD_RESULT lines are authoritative for command completion. These
# human-readable lines remain useful for display and backward diagnostics.
RESULT_HOLD_OK = "[SESSION] Host session held: ALL OK"
RESULT_HOLD_REFUSED = "[SESSION] NOT ALL OK - HOLD refused."
RESULT_KEEPALIVE_OK = ": lease renewed for "
RESULT_KEEPALIVE_FAILED = "[SESSION] NOT ALL OK - lease NOT renewed."
RESULT_RELEASED_OK = "[SESSION] Released: ALL OK"
RESULT_LEASE_EXPIRED = "[SESSION] Host lease EXPIRED after"

# THE FIRMWARE IS AUTHORITATIVE for its own lease length. This mirror exists
# only so the host can reason about margin, and it must never be treated as
# the definition. If they disagree, the firmware wins and the host's keepalive
# cadence is what has to change.
FIRMWARE_LEASE_SECONDS = 15.0

# Three keepalives fit inside one lease, so two can be lost without the session
# dropping.
KEEPALIVE_INTERVAL_SECONDS = 5.0

# How long to wait for the firmware's machine ACK before calling a command
# unheard.
ACK_TIMEOUT_SECONDS = 4.0

# Response capture. The idle threshold sits below the firmware's 1-second CSV
# cadence and its 5-second heartbeat, so a finished response reads as quiet
# whether or not loop() is running.
RESPONSE_IDLE_SECONDS = 0.6
RESPONSE_CAPTURE_SECONDS = 2.0

# LOGGER STORAGE DUMP streams one line per stored record with no pacing.
RESPONSE_CAPTURE_SECONDS_DUMP = 10.0

# How often to look for a port while waiting for a rendezvous.
PORT_POLL_SECONDS = 0.2

# How often to say "still waiting" during a long wait.
WAIT_HEARTBEAT_SECONDS = 15.0

SERIAL_READ_TIMEOUT_SECONDS = 0.1


# ============================================================================
# WHICH COMMANDS MAY WAIT FOR A RENDEZVOUS
# ============================================================================
#
# An autonomously sleeping board has no USB device between wakes, so "the port
# is missing" is the normal state and waiting for the next rendezvous is the
# normal response. Two commands are different.
#
# KEEPALIVE and RELEASE both address a session that is ALREADY held. A session
# lives on one transport; when that transport disappears, the session is over -
# the firmware's 15-second lease expires on its own and the board returns to
# autonomous sleep with no help from the host (D-025).
#
# So waiting is not merely useless for those two, it is wrong. The board that
# eventually appears is a board that has slept, woken, and started a new
# rendezvous. Sending a RELEASE saved up from a session that ended minutes ago
# either lands on a board holding no session at all - harmless, the firmware
# answers NOT_HELD - or, if something else claimed that new wake in between, it
# releases a session it never held. Sleeping a board out from under another
# process is exactly the kind of silent misbehavior this project refuses.
#
# HOLD is not in this set. HOLD creates a session rather than addressing one,
# so any rendezvous will do and waiting for the next is precisely right.

SESSION_SCOPED_COMMANDS = frozenset(
    {
        CMD_SESSION_KEEPALIVE,
        CMD_SESSION_RELEASE,
    }
)


def requires_live_session(command: str) -> bool:
    """True when `command` addresses a session that must already exist."""

    return command.strip().upper() in SESSION_SCOPED_COMMANDS


def ack_line_for(command: str) -> str:
    """The exact machine line the firmware emits when it parses `command`.

    The firmware uppercases a command before emission.
    """

    return f"{CMD_ACK_PREFIX}{command.strip().upper()}"


def result_prefix_for(command: str) -> str:
    """Prefix for the machine result line for `command`."""

    return f"{CMD_RESULT_PREFIX}{command.strip().upper()},"


def capture_seconds_for(command: str) -> float:
    """How long to keep reading a response for this command."""

    if "STORAGE DUMP" in command.strip().upper():
        return RESPONSE_CAPTURE_SECONDS_DUMP

    return RESPONSE_CAPTURE_SECONDS


class DeviceError(Exception):
    """A device or protocol failure that the caller must not ignore."""


# ============================================================================
# PORT DISCOVERY
# ============================================================================


def find_port() -> Optional[str]:
    """Return the current board port, or None when the board is not present.

    Rediscovered every time rather than remembered. A board that wakes from
    deep sleep can enumerate under a different usbmodem number, so a path that
    was right one rendezvous ago may name nothing at the next.
    """

    ports = sorted(glob.glob(PORT_GLOB))

    if not ports:
        return None

    return ports[0]


def wait_for_port(
    timeout: Optional[float] = None,
    poll_seconds: float = PORT_POLL_SECONDS,
    on_first_wait: Optional[Callable[[], None]] = None,
    on_heartbeat: Optional[Callable[[float], None]] = None,
    heartbeat_seconds: float = WAIT_HEARTBEAT_SECONDS,
    should_continue: Optional[Callable[[], bool]] = None,
) -> Optional[str]:
    """Block until the board appears, and return its port.

    A missing port is an ORDINARY state, not an error: the board exposes USB
    only during the rendezvous window that follows each autonomous timer wake.
    Callers that treat absence as a failure are the bug this exists to prevent.

    `timeout` of None waits indefinitely, which is usually right: one wait can
    legitimately last longer than the autonomous cadence. Returns None if a
    timeout elapses or `should_continue` asks to stop.
    """

    started = time.monotonic()
    announced = False
    next_heartbeat = heartbeat_seconds

    while True:
        port = find_port()

        if port is not None:
            return port

        if should_continue is not None and not should_continue():
            return None

        if not announced:
            announced = True

            if on_first_wait is not None:
                on_first_wait()

        elapsed = time.monotonic() - started

        if timeout is not None and elapsed >= timeout:
            return None

        if on_heartbeat is not None and elapsed >= next_heartbeat:
            next_heartbeat += heartbeat_seconds
            on_heartbeat(elapsed)

        time.sleep(poll_seconds)


# ============================================================================
# ONE-SHOT SERIAL USE
# ============================================================================


@dataclass
class CommandResult:
    """Outcome of one command sent over a port this process owns.

    Three different facts, deliberately not collapsed into one:

        acknowledged   the firmware PARSED the command (CMD_ACK seen)
        result_seen    the firmware REPORTED an outcome (any CMD_RESULT seen)
        completed      that outcome was OK

    `result_seen` and `completed` differ whenever the firmware answers
    CMD_RESULT,<cmd>,ERROR - a command that ran and failed. That is a real
    answer and capture should end on it, which is why termination keys on
    `result_seen` while success keys on `completed`.
    """

    command: str
    acknowledged: bool
    completed: bool = False
    result_line: str = ""
    transport_lost_after_result: bool = False
    lines: list = field(default_factory=list)
    stop_reason: str = ""
    truncated: bool = False

    @property
    def result_seen(self) -> bool:
        """True once the firmware has reported any outcome for this command."""

        return bool(self.result_line)

    @property
    def ack_line(self) -> str:
        return ack_line_for(self.command)


class SerialDevice:
    """Owns a serial port for one-shot command work.

    Only for callers that are the sole owner of the port. solar_logger.py must
    NOT use this while it is connected: two readers on one device fight over
    the same bytes. It uses SessionClient instead.
    """

    def __init__(self, port: str, baud: int = BAUD_RATE, log: Callable[[str], None] = print):
        self.port = port
        self.baud = baud
        self.log = log
        self._serial: Optional[serial.Serial] = None

    def open(self) -> None:
        try:
            self._serial = serial.Serial(
                port=self.port,
                baudrate=self.baud,
                timeout=SERIAL_READ_TIMEOUT_SECONDS,
            )

        except (serial.SerialException, OSError) as error:
            raise DeviceError(f"Could not open {self.port}: {error}") from error

    def close(self) -> None:
        if self._serial is None:
            return

        try:
            self._serial.close()

        except (serial.SerialException, OSError) as error:
            # Reported rather than swallowed: a port that would not close is a
            # real problem for whatever tries to open it next.
            self.log(f"[DEVICE] WARNING: Could not close {self.port}: {error}")

        finally:
            self._serial = None

    def __enter__(self) -> "SerialDevice":
        self.open()
        return self

    def __exit__(self, *_exc) -> None:
        self.close()

    def send_command(
        self,
        command: str,
        ack_timeout: float = ACK_TIMEOUT_SECONDS,
        capture_seconds: Optional[float] = None,
        idle_seconds: float = RESPONSE_IDLE_SECONDS,
    ) -> CommandResult:
        """Send one command, require machine ACK/RESULT, then capture reply.

        Writing bytes to a serial device proves the host accepted them and
        nothing more. `acknowledged` is True only when the firmware's machine
        ACK is observed, and `completed` requires its machine RESULT.
        """

        if self._serial is None:
            raise DeviceError("send_command() called on a closed device")

        command = command.strip()

        if not command:
            raise DeviceError("refusing to send an empty command")

        if capture_seconds is None:
            capture_seconds = capture_seconds_for(command)

        expected = ack_line_for(command)
        expected_result_prefix = result_prefix_for(command)
        result = CommandResult(command=command, acknowledged=False)

        try:
            self._serial.reset_input_buffer()
            self._serial.write((command + "\n").encode("utf-8"))
            self._serial.flush()

        except (serial.SerialException, OSError) as error:
            raise DeviceError(f"Could not write to {self.port}: {error}") from error

        # --- wait for the machine ACK ------------------------------------
        deadline = time.monotonic() + ack_timeout
        pre_ack_lines = []

        while time.monotonic() < deadline:
            line = self._read_line()

            if line is None:
                continue

            if line.strip() == expected:
                result.acknowledged = True
                break

            pre_ack_lines.append(line)

        if not result.acknowledged:
            result.lines = pre_ack_lines
            result.stop_reason = "no acknowledgement"
            return result

        # --- capture the response ----------------------------------------
        capture_deadline = time.monotonic() + capture_seconds
        last_data_at = time.monotonic()
        result.stop_reason = "hard cap"

        while time.monotonic() < capture_deadline:
            try:
                line = self._read_line()

            except DeviceError as error:
                if result.result_seen:
                    result.transport_lost_after_result = True
                    result.stop_reason = f"transport lost after result: {error}"
                    break

                raise

            if line is None:
                if result.result_seen and time.monotonic() - last_data_at >= idle_seconds:
                    result.stop_reason = "quiet after result"
                    break

                continue

            result.lines.append(line)
            last_data_at = time.monotonic()

            if line.startswith(expected_result_prefix):
                result.completed = line == f"{expected_result_prefix}OK"
                result.result_line = line

        # Truncation means the capture window ended before the firmware said how
        # the command turned out. A command that answered ERROR was not
        # truncated: it reported, and the report was a failure. Keying this on
        # `completed` made every failed command also claim its output might be
        # incomplete, which pointed the reader at the wrong problem.
        result.truncated = (
            result.stop_reason == "hard cap" and not result.result_seen
        )
        return result

    def _read_line(self) -> Optional[str]:
        """One line, or None when the read timed out with nothing.

        A closed port is a DeviceError rather than an AttributeError, so a
        caller that lost the device mid-capture gets the same failure it would
        get from any other read problem instead of a traceback naming NoneType.
        """

        if self._serial is None:
            raise DeviceError(f"_read_line() called with {self.port} closed")

        try:
            raw = self._serial.readline()

        except (serial.SerialException, OSError) as error:
            raise DeviceError(f"Read failed on {self.port}: {error}") from error

        if not raw:
            return None

        return raw.decode("utf-8", errors="replace").rstrip("\r\n")


# ============================================================================
# HOST SESSION LEASE
# ============================================================================


class SessionClient:
    """Host end of the firmware's SESSION lease. Does NOT own the port.

    The caller writes through `write_line` and feeds every received line to
    `note_line`. That split exists because solar_logger.py runs its own read
    loop; a session layer that read the port itself would steal the logger's
    telemetry.

    A session is a lease held over a transport. USB is the only transport
    implemented, and nothing here should assume it is the only one there will
    ever be.
    """

    def __init__(
        self,
        write_line: Callable[[str], None],
        log: Callable[[str], None] = print,
        transport: Transport = Transport.USB,
        keepalive_interval: float = KEEPALIVE_INTERVAL_SECONDS,
        ack_timeout: float = ACK_TIMEOUT_SECONDS,
    ):
        self.write_line = write_line
        self.log = log
        self.transport = transport
        self.keepalive_interval = keepalive_interval
        self.ack_timeout = ack_timeout

        self.held = False

        # When the lease was granted. The keepalive schedule falls back to this
        # rather than to the echo timestamp, because the echo can be missed in a
        # burst while the outcome line still arrives. Keying the first keepalive
        # on the echo meant a session that was genuinely held would never send
        # one, and would then expire 15 seconds later for no reason.
        self.held_since: Optional[float] = None

        self.last_ack_at: Optional[float] = None
        self.keepalives_sent = 0
        self.keepalives_acked = 0

        self._pending: Optional[tuple] = None
        self._last_keepalive_at: Optional[float] = None
        self._awaiting_outcome: Optional[str] = None

        # None means "not known yet". Set False the moment the firmware refuses
        # a hold, which is how a non-autonomous board announces itself.
        self.autonomous_armed: Optional[bool] = None

    # -- outgoing ---------------------------------------------------------

    def _send(self, command: str) -> None:
        if self._pending is not None:
            pending_command, _ = self._pending

            # Never silent. An unanswered command being replaced means the
            # firmware did not echo the previous one, and the caller has to
            # know its session state is not what it thinks.
            self.log(
                f"[SESSION] WARNING: {pending_command} was never acknowledged; "
                f"sending {command} anyway."
            )

        self._pending = (command, time.monotonic())
        self.write_line(command)

    def request_hold(self) -> None:
        self.log("[SESSION] Claiming the board with LOGGER SESSION HOLD...")
        self._send(CMD_SESSION_HOLD)

    def request_keepalive(self) -> None:
        self.keepalives_sent += 1
        self._last_keepalive_at = time.monotonic()
        self._send(CMD_SESSION_KEEPALIVE)

    def request_release(self) -> None:
        self.log("[SESSION] Releasing the board with LOGGER SESSION RELEASE...")
        self._send(CMD_SESSION_RELEASE)

    # -- incoming ---------------------------------------------------------

    def note_line(self, line: str) -> bool:
        """Feed one received firmware line in.

        Returns True if the line was meaningful to the session. Two different
        kinds of line matter and they mean different things:

          the echo      the command was PARSED
          an outcome    the command SUCCEEDED or FAILED

        Keying session state on the echo alone would be wrong. processCommand()
        prints the echo before dispatching, so the firmware echoes a HOLD it is
        about to refuse for not being in autonomous mode. Believing that echo
        would leave this client convinced it holds a lease that was never
        granted, and then reporting a session the board does not have.
        """

        text = line.strip()
        consumed = False

        # --- machine ACK: the command was parsed -------------------------
        if self._pending is not None:
            command, _sent_at = self._pending

            if text == ack_line_for(command):
                self._pending = None
                self.last_ack_at = time.monotonic()
                self._awaiting_outcome = command
                consumed = True

        # --- machine RESULT: the command completed -----------------------
        #
        # The outcome is accepted for a command still awaiting its ACK as well as
        # for one whose ACK arrived. D-036 is explicit that the ACK can be lost:
        # with setTxTimeoutMs(0) a write returns short when the TX ring is full,
        # and Arduino's Print does not expose that. Requiring the ACK first meant
        # a dropped ACK also discarded the RESULT that followed it, so a lease the
        # firmware had genuinely granted was never recorded here - and the next
        # keepalive would then be refused by a board this client thought it had
        # never claimed.
        #
        # Matching on the result line's own command text is what makes this safe:
        # it still cannot be confused with another command's outcome.
        expecting = self._awaiting_outcome

        if expecting is None and self._pending is not None:
            expecting = self._pending[0]

        if expecting is not None:
            command = expecting
            prefix = result_prefix_for(command)

            if text.startswith(prefix):
                status = text[len(prefix):]
                self._awaiting_outcome = None
                self._pending = None

                if command == CMD_SESSION_HOLD:
                    self.held = status == "OK"
                    self.held_since = time.monotonic() if self.held else None
                    self.autonomous_armed = status != "REFUSED"

                elif command == CMD_SESSION_KEEPALIVE:
                    if status == "OK":
                        self.keepalives_acked += 1
                        self._last_keepalive_at = time.monotonic()

                elif command == CMD_SESSION_RELEASE:
                    self.held = False

                self.log(
                    f"[SESSION] Firmware completed {command}: {status}"
                )
                return True

        # --- outcome: what actually happened -----------------------------
        if text == RESULT_HOLD_OK:
            self.held = True
            self.held_since = time.monotonic()
            self.autonomous_armed = True
            self._awaiting_outcome = None
            self.log(
                f"[SESSION] Host session acquired over {self.transport.name}: "
                "ALL OK"
            )
            self.log(
                "[SESSION] Autonomous deep sleep is suspended while this "
                "logger is connected."
            )
            return True

        if text == RESULT_HOLD_REFUSED:
            self.held = False
            self._awaiting_outcome = None
            self.autonomous_armed = False
            self.log(
                "[SESSION] Firmware REFUSED the hold: autonomous mode is not "
                "armed."
            )
            self.log(
                "[SESSION] No lease is held and none is needed. The board is "
                "not going to sleep on its own."
            )
            return True

        if RESULT_KEEPALIVE_OK in text:
            self.keepalives_acked += 1
            self._awaiting_outcome = None

            # A renewed lease is also proof the session is alive, so it resets
            # the schedule baseline even if this client never saw the echo.
            self._last_keepalive_at = time.monotonic()
            return True

        if text == RESULT_KEEPALIVE_FAILED:
            self._awaiting_outcome = None
            self.held = False
            self.log(
                "[SESSION] ERROR: The firmware says no host session is held, "
                "so the KEEPALIVE renewed nothing."
            )
            self.log(
                "[SESSION] The lease has probably already expired. A new "
                "LOGGER SESSION HOLD is required."
            )
            return True

        if text == RESULT_RELEASED_OK:
            self.held = False
            self._awaiting_outcome = None
            self.log("[SESSION] Release acknowledged by firmware: ALL OK")
            return True

        if text.startswith(RESULT_LEASE_EXPIRED):
            self.held = False
            self._awaiting_outcome = None
            self.log(
                "[SESSION] The firmware expired our lease. It is returning to "
                "autonomous sleep."
            )
            return True

        return consumed

    # -- scheduling -------------------------------------------------------

    def due_for_keepalive(self, now: Optional[float] = None) -> bool:
        if not self.held:
            return False

        if now is None:
            now = time.monotonic()

        baseline = self._last_keepalive_at

        if baseline is None:
            baseline = self.held_since

        if baseline is None:
            # held is True but nothing recorded when. Send one rather than
            # stall: an extra keepalive is harmless, a missing one ends the
            # session.
            return True

        return now - baseline >= self.keepalive_interval

    def check_pending_timeout(self, now: Optional[float] = None) -> Optional[str]:
        """Surface a command the firmware never echoed.

        Returns the unacknowledged command name once, then clears it, so a
        caller polling this cannot be spammed but also cannot miss it.
        """

        if self._pending is None:
            return None

        if now is None:
            now = time.monotonic()

        command, sent_at = self._pending

        if now - sent_at < self.ack_timeout:
            return None

        self._pending = None

        self.log(
            f"[SESSION] ERROR: {command} was NOT acknowledged within "
            f"{self.ack_timeout:.0f}s."
        )

        if command == CMD_SESSION_KEEPALIVE:
            self.log(
                "[SESSION] The lease may already have expired. The board can "
                "return to autonomous sleep at any moment."
            )

        elif command == CMD_SESSION_HOLD:
            self.held = False
            self.log(
                "[SESSION] The board was NOT claimed. It may sleep again when "
                "its rendezvous window closes."
            )

        return command

    def forget_session(self, reason: str) -> None:
        """Drop local session state after the link is gone.

        Never implies the firmware released anything: only that this process
        can no longer speak for the session.
        """

        if self.held:
            self.log(
                f"[SESSION] Host session state dropped ({reason}). The "
                "firmware lease expires on its own if it was still held."
            )

        self.held = False
        self.held_since = None
        self._pending = None
        self._last_keepalive_at = None
