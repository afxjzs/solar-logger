#!/usr/bin/env python3

"""
Command-line front end for the shared host/device protocol.

tools/send.sh and tools/upload.sh both call this, so neither has to carry its
own copy of port discovery, rendezvous waiting, or acknowledgement handling.
All of that lives in app/device_session.py; this file is argument parsing and
terminal output.

Subcommands:

    find-port           print the current port, or exit 1 if the board is away
    wait-port           block until the board appears, then print its port
    send COMMAND ...    send one command, require machine ACK and RESULT,
                        print whatever the firmware says back

WAITING IS THE DEFAULT

An autonomously sleeping board has no USB device between wakes, so a missing
port is an ordinary state and `send` waits for the next rendezvous. `--no-wait`
asks for immediate failure instead, which is what tools/upload.sh needs inside
its own retry loop.

The exception is SESSION KEEPALIVE and SESSION RELEASE. Those address a session
that already exists, so they never wait and an explicit `--wait` is refused.
resolve_wait() explains why in full.

Exit status is meaningful in every mode:

    0   the firmware acknowledged and completed the command
    1   it did not, or the board was absent and waiting was not allowed
    2   the request itself was refused before anything was sent
    130 cancelled with Control-C

so a retry loop keeps retrying until the command actually lands.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))

import device_session as ds  # noqa: E402

PROJECT_ROOT = Path(__file__).resolve().parent.parent
SERIAL_COMMAND_FILE = PROJECT_ROOT / ".serial-command"
UPLOAD_HANDSHAKE_FILE = PROJECT_ROOT / ".upload-in-progress"


def out(message: str = "") -> None:
    print(message, flush=True)


def err(message: str) -> None:
    print(message, file=sys.stderr, flush=True)


# ============================================================================
# find-port / wait-port
# ============================================================================


def cmd_find_port(_args) -> int:
    port = ds.find_port()

    if port is None:
        err("[DEVICE] No board port is currently visible.")
        err("[DEVICE] The board may be in autonomous deep sleep.")
        return 1

    out(port)
    return 0


def cmd_wait_port(args) -> int:
    def first_wait():
        err("[DEVICE] No board port is currently visible.")
        err("[DEVICE] The board may be in autonomous deep sleep.")
        err("[DEVICE] Waiting for the next USB rendezvous...")
        err("[DEVICE] Press Control-C to cancel.")

    def heartbeat(elapsed):
        err(f"[DEVICE] Still waiting for a rendezvous ({elapsed:.0f}s elapsed)...")

    port = ds.wait_for_port(
        timeout=args.timeout,
        poll_seconds=args.poll_seconds,
        on_first_wait=first_wait,
        on_heartbeat=heartbeat,
        heartbeat_seconds=args.heartbeat_seconds,
    )

    if port is None:
        err(f"[DEVICE] NOT ALL OK - no board appeared within {args.timeout}s.")
        return 1

    out(port)
    return 0


# ============================================================================
# send
# ============================================================================


def logger_owns_serial() -> bool:
    """True when app/solar_logger.py is running and owns the port."""

    import subprocess

    result = subprocess.run(
        ["pgrep", "-f", r"(^|[[:space:]/])app/solar_logger\.py([[:space:]]|$)"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )

    return result.returncode == 0


def queue_for_logger(command: str) -> int:
    """Hand the command to the running logger instead of opening the port.

    The logger owns Serial in this path. Opening it here as a second reader is
    exactly the multi-owner problem the command file exists to avoid, so this
    deliberately captures nothing and says so.
    """

    out("[SERIAL] Logger detected. Handing command to logger.")

    try:
        # x mode fails rather than overwriting another pending command.
        with SERIAL_COMMAND_FILE.open("x", encoding="utf-8") as handle:
            handle.write(command + "\n")

    except FileExistsError:
        err("[COMMAND] ERROR: A command is already pending.")
        return 1

    except OSError as error:
        err(f"[COMMAND] ERROR: Could not queue command: {error}")
        return 1

    out("[COMMAND] Queued for logger.")
    out("[COMMAND] Firmware acknowledgement not observed by this tool.")
    out("[COMMAND] Response not captured here: the logger owns the serial port.")
    out(f"[COMMAND] Watch the logger console for: {ds.ack_line_for(command)}")
    return 0


def resolve_wait(command: str, requested) -> Optional[bool]:
    """Decide whether this command may wait for the next USB rendezvous.

    `requested` is three-valued on purpose:

        None    nothing was asked for; use the rule below
        True    --wait was given explicitly
        False   --no-wait was given explicitly

    THE RULE

        ordinary commands, STATUS, STORAGE, AUTONOMOUS, SESSION HOLD
            wait by default

        SESSION KEEPALIVE, SESSION RELEASE
            never wait, and refuse an explicit --wait

    Returns None when the request is refused, and says why first. A refusal is
    not the same as "do not wait": the caller exits rather than sending
    something that could act on the wrong session.
    """

    session_scoped = ds.requires_live_session(command)

    if not session_scoped:
        return True if requested is None else bool(requested)

    if requested is True:
        err(f"[SERIAL] ERROR: --wait is refused for {command}.")
        err("[SERIAL] It addresses a session that is already held, and a session lives")
        err("[SERIAL] on one transport. Waiting for the next rendezvous means waiting for")
        err("[SERIAL] a board that has since slept, woken, and started a NEW session -")
        err("[SERIAL] which this command would then act on by mistake.")
        err("[SERIAL] Send it now while the port exists, or take a new session with")
        err("[SERIAL] LOGGER SESSION HOLD, which may wait.")
        return None

    return False


def cmd_send(args) -> int:
    command = " ".join(args.command).strip()

    if not command:
        err("[COMMAND] ERROR: No command given.")
        return 1

    if UPLOAD_HANDSHAKE_FILE.exists():
        err("[ERROR] Firmware upload is in progress. Command not sent.")
        return 1

    out("[COMMAND] BMW Solar Logger command sender")
    out(f"[COMMAND] Command: {command}")

    if logger_owns_serial():
        return queue_for_logger(command)

    out("[SERIAL] Logger not running. Using direct serial access.")

    # Waiting is the default, because an autonomously sleeping board having no
    # USB device is the normal state. See resolve_wait() for the one exception
    # and why it exists.
    wait_for_rendezvous = resolve_wait(command, args.wait)

    if wait_for_rendezvous is None:
        return 2

    port = ds.find_port()

    if port is None:
        if not wait_for_rendezvous:
            err("[SERIAL] ERROR: No board port found.")

            if ds.requires_live_session(command):
                err(f"[SERIAL] {command} addresses a session that is already held.")
                err("[SERIAL] The port it was held on is gone, so that session is over:")
                err("[SERIAL] the firmware lease expires on its own and the board resumes")
                err("[SERIAL] autonomous sleep. There is nothing to send this to.")
                err("[SERIAL] To take a NEW session: tools/send.sh LOGGER SESSION HOLD")
            else:
                err("[SERIAL] An autonomously sleeping board has no USB device between wakes.")
                err("[SERIAL] Drop --no-wait to wait for the next rendezvous.")

            return 1

        def first_wait():
            out("[SERIAL] No board port is currently visible.")
            out("[SERIAL] The board may be in autonomous deep sleep.")
            out("[SERIAL] Waiting for the next USB rendezvous...")
            out("[SERIAL] Press Control-C to cancel.")

        def heartbeat(elapsed):
            out(f"[SERIAL] Still waiting ({elapsed:.0f}s elapsed)...")

        port = ds.wait_for_port(
            timeout=args.timeout,
            on_first_wait=first_wait,
            on_heartbeat=heartbeat,
        )

        if port is None:
            err(f"[SERIAL] NOT ALL OK - no board appeared within {args.timeout}s.")
            return 1

    out(f"[SERIAL] Port: {port}")

    capture = args.capture_seconds
    if capture is None:
        capture = ds.capture_seconds_for(command)

    try:
        with ds.SerialDevice(port, log=err) as device:
            out("[COMMAND] Bytes written to serial port.")
            out(f"[COMMAND] Waiting up to {ds.ACK_TIMEOUT_SECONDS:.0f}s for firmware ACK...")

            result = device.send_command(
                command,
                ack_timeout=ds.ACK_TIMEOUT_SECONDS,
                capture_seconds=capture,
                idle_seconds=args.idle_seconds,
            )

    except ds.DeviceError as error:
        err(f"[COMMAND] NOT ALL OK - {error}")
        err("[COMMAND] The board may have returned to deep sleep mid-command.")
        return 1

    except KeyboardInterrupt:
        err("")
        err("[COMMAND] Cancelled by user.")
        return 130

    if not result.acknowledged:
        err("[COMMAND] NOT ALL OK - firmware parsed-command ACK NOT observed.")
        err("[COMMAND] Bytes were written, but parsing was not confirmed.")
        err("[COMMAND] An armed autonomous board only parses commands while awake:")
        err("[COMMAND] during a USB rendezvous or its cold-boot maintenance window.")

        if result.lines:
            err("[COMMAND] --- firmware output seen while waiting ---")
            for line in result.lines[-15:]:
                err(line)
            err("[COMMAND] --- end ---")
        else:
            err("[COMMAND] No firmware output was received at all.")

        return 1

    out(f"[COMMAND] Firmware parsed command: ALL OK ({result.ack_line})")

    if not result.completed:
        # Two different failures, and telling them apart is the point. The
        # firmware reporting ERROR is an answer; no result arriving at all is
        # the absence of one, and they need different next steps.
        if result.result_seen:
            err("[COMMAND] NOT ALL OK - the firmware RAN the command and it FAILED.")
            err(f"[COMMAND] Firmware reported: {result.result_line}")
        else:
            err("[COMMAND] NOT ALL OK - firmware completion RESULT NOT observed.")
            err("[COMMAND] The command was parsed, but completion was not confirmed.")

        # The firmware prints exactly why it refused or failed, so showing the
        # captured output matters more here than on the success path. Returning
        # before printing it left the operator with a failure and no reason.
        if result.lines:
            err("[COMMAND] --- firmware response ---")
            for line in result.lines:
                err(line)
            err("[COMMAND] --- end of firmware response ---")

        return 1

    out(f"[COMMAND] Firmware completed command: ALL OK ({result.result_line})")
    out(
        f"[COMMAND] Capturing response: quiet for {args.idle_seconds * 1000:.0f}ms "
        f"ends it, {capture:.0f}s hard cap."
    )
    out("[COMMAND] --- firmware response ---")

    for line in result.lines:
        out(line)

    out("[COMMAND] --- end of firmware response ---")

    if not result.lines:
        out("[COMMAND] The firmware acknowledged the command but sent no response lines.")
        out("[COMMAND] That is expected for a command whose only output is the echo.")
    else:
        out(f"[COMMAND] Response lines captured: {len(result.lines)}")

    if result.truncated:
        err(f"[COMMAND] Capture ended: {capture:.0f}s hard cap reached.")
        err("[COMMAND] WARNING: output may be TRUNCATED. The firmware was still")
        err("[COMMAND] sending. Re-run with --capture-seconds <n> for longer.")
    else:
        if result.transport_lost_after_result:
            out(
                "[COMMAND] Capture ended: transport disappeared after the "
                "successful result (expected for RELEASE)."
            )
        else:
            out(
                f"[COMMAND] Capture ended: port quiet for "
                f"{args.idle_seconds * 1000:.0f}ms."
            )

    # Both machine protocol stages were observed. A later transport loss is
    # reported accurately by the capture stop reason and does not change the
    # already-confirmed command result.
    return 0


# ============================================================================
# session
# ============================================================================
#
# So callers name an ACTION rather than retyping a protocol string. The command
# text itself stays in device_session.py, which is the point of the module.

SESSION_ACTIONS = {
    "hold": ds.CMD_SESSION_HOLD,
    "keepalive": ds.CMD_SESSION_KEEPALIVE,
    "release": ds.CMD_SESSION_RELEASE,
    "status": ds.CMD_SESSION_STATUS,
}

AUTONOMOUS_ACTIONS = {
    "on": ds.CMD_AUTONOMOUS_ON,
    "off": ds.CMD_AUTONOMOUS_OFF,
    "status": ds.CMD_AUTONOMOUS_STATUS,
}


def cmd_session(args) -> int:
    args.command = SESSION_ACTIONS[args.action].split()
    return cmd_send(args)


def cmd_autonomous(args) -> int:
    args.command = AUTONOMOUS_ACTIONS[args.action].split()
    return cmd_send(args)


# ============================================================================


def add_wait_arguments(parser: argparse.ArgumentParser) -> None:
    """--wait / --no-wait, defaulting to neither.

    The default is None rather than True so resolve_wait() can tell "the user
    said nothing" from "the user asked for this". They lead to different
    behavior for SESSION KEEPALIVE and SESSION RELEASE, and a two-valued flag
    would have to silently ignore one of them.
    """

    group = parser.add_mutually_exclusive_group()

    group.add_argument(
        "--wait",
        dest="wait",
        action="store_true",
        default=None,
        help="wait for the next USB rendezvous (already the default)",
    )

    group.add_argument(
        "--no-wait",
        dest="wait",
        action="store_false",
        default=None,
        help="fail immediately when the board is asleep, instead of waiting",
    )


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        prog="device_tool.py",
        description="Host-side device and session control for the BMW Solar Logger.",
    )

    sub = parser.add_subparsers(dest="subcommand", required=True)

    p_find = sub.add_parser("find-port", help="print the current board port")
    p_find.set_defaults(func=cmd_find_port)

    p_wait = sub.add_parser("wait-port", help="wait for the board, then print its port")
    p_wait.add_argument("--timeout", type=float, default=None,
                        help="seconds to wait; default is forever")
    # tools/upload.sh passes these from UPLOAD_PORT_POLL_SECONDS and
    # UPLOAD_WAIT_HEARTBEAT_SECONDS. Both were documented as tunables and both
    # had silently stopped doing anything when waiting moved into this module:
    # the shell read them into variables nothing used.
    p_wait.add_argument("--poll-seconds", type=float, default=ds.PORT_POLL_SECONDS,
                        help="how often to look for the port while waiting")
    p_wait.add_argument("--heartbeat-seconds", type=float,
                        default=ds.WAIT_HEARTBEAT_SECONDS,
                        help="how often to print a still-waiting notice")
    p_wait.set_defaults(func=cmd_wait_port)

    p_send = sub.add_parser("send", help="send one command and print the response")
    p_send.add_argument("command", nargs="+")
    add_wait_arguments(p_send)
    p_send.add_argument("--timeout", type=float, default=None,
                        help="seconds to wait for a rendezvous; default is forever")
    p_send.add_argument("--capture-seconds", type=float, default=None,
                        help="response capture hard cap; default depends on the command")
    p_send.add_argument("--idle-seconds", type=float, default=ds.RESPONSE_IDLE_SECONDS,
                        help="quiet period that ends response capture")
    p_send.set_defaults(func=cmd_send)

    p_session = sub.add_parser(
        "session",
        help="send a SESSION command by name instead of by protocol string",
    )
    p_session.add_argument("action", choices=sorted(SESSION_ACTIONS))
    add_wait_arguments(p_session)
    p_session.add_argument("--timeout", type=float, default=None)
    p_session.add_argument("--capture-seconds", type=float, default=None)
    p_session.add_argument("--idle-seconds", type=float, default=ds.RESPONSE_IDLE_SECONDS)
    p_session.set_defaults(func=cmd_session)

    p_auto = sub.add_parser(
        "autonomous",
        help="enable, disable, or query autonomous sleep/wake/store mode",
    )
    p_auto.add_argument("action", choices=sorted(AUTONOMOUS_ACTIONS))
    add_wait_arguments(p_auto)
    p_auto.add_argument("--timeout", type=float, default=None)
    p_auto.add_argument("--capture-seconds", type=float, default=None)
    p_auto.add_argument("--idle-seconds", type=float, default=ds.RESPONSE_IDLE_SECONDS)
    p_auto.set_defaults(func=cmd_autonomous)

    args = parser.parse_args(argv)

    try:
        return args.func(args)

    except KeyboardInterrupt:
        err("")
        err("[DEVICE] Cancelled by user.")
        return 130


if __name__ == "__main__":
    sys.exit(main())
