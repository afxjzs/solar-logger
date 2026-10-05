"""Run one canonical hardware command and preserve exactly what it printed.

    uv run python tools/capture-command.py <evidence-dir> <label> tools/send.sh [OPTION ...] COMMAND
    uv run python tools/capture-command.py <evidence-dir> <label> tools/upload.sh

Writes two files into the evidence directory:

    <label>.txt    the child's combined stdout/stderr, byte for byte
    <label>.json   argv, host start/finish times, exit status, bytes, SHA-256

The wrapper never opens the serial port itself and never alters the tool it
runs. It captures what the tool emitted, so a capture is evidence of the tool's
output and not of anything this script decided.

It refuses to overwrite an existing capture. A hardware step that has to be
repeated gets a new label, so the record keeps the first attempt rather than
quietly replacing it with the one that worked.

It also refuses to pass a destructive command to tools/send.sh. RESET, storage
CLEAR, arming and interval changes are deliberate operations with their own
consequences for a live experiment, and none of them belongs in an acceptance
capture that is supposed to observe without changing.

Exits with the child's exit status, so a caller can test it.
"""

import datetime
import hashlib
import json
import os
import pathlib
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent

ALLOWED_TOOLS = ("tools/send.sh", "tools/upload.sh")

# Substrings that mark a command as changing the board rather than observing it.
# ' ON' and ' OFF' carry their leading space so that VERSION and similar are not
# caught by accident.
DESTRUCTIVE = ("CLEAR", "RESET", " OFF", " ON", "INTERVAL")

# Enough of the user's PATH for uv and the Homebrew tools the scripts call.
EXTRA_PATH = "/Users/afxjzs/.local/bin:/opt/homebrew/bin:"


def main(argv: list[str]) -> int:
    if len(argv) < 4:
        print(__doc__, file=sys.stderr)
        return 2

    evidence = pathlib.Path(argv[1])
    label = argv[2]
    args = argv[3:]

    if not label.replace("-", "").isalnum():
        print(f"[CAPTURE] ERROR: label {label!r} is not alphanumeric-with-dashes.",
              file=sys.stderr)
        return 2

    if args[0] not in ALLOWED_TOOLS:
        print(f"[CAPTURE] ERROR: {args[0]!r} is not a canonical hardware tool. "
              f"Use one of {ALLOWED_TOOLS}.", file=sys.stderr)
        return 2

    if args[0] == "tools/send.sh":
        command = " ".join(args)
        found = [x for x in DESTRUCTIVE if x in command]
        if found:
            print(f"[CAPTURE] ERROR: refusing to capture a destructive command; "
                  f"matched {found}.", file=sys.stderr)
            return 2

    evidence.mkdir(parents=True, exist_ok=True)
    raw = evidence / f"{label}.txt"
    meta = evidence / f"{label}.json"

    if raw.exists() or meta.exists():
        print(f"[CAPTURE] ERROR: {label} already captured. Refusing to overwrite; "
              f"use a new label so the first attempt survives.", file=sys.stderr)
        return 2

    started = datetime.datetime.now().astimezone().isoformat()

    env = os.environ.copy()
    env["PATH"] = EXTRA_PATH + env.get("PATH", "")
    env["PYTHONUNBUFFERED"] = "1"

    with raw.open("xb") as handle:
        process = subprocess.Popen(args, cwd=REPO, env=env, stdout=handle,
                                   stderr=subprocess.STDOUT)
        print(f"[CAPTURE] {label}: child PID {process.pid}, started {started}",
              flush=True)
        status = process.wait()

    payload = raw.read_bytes()
    metadata = {
        "started": started,
        "finished": datetime.datetime.now().astimezone().isoformat(),
        "argv": args,
        "exit_code": status,
        "bytes": len(payload),
        "sha256": hashlib.sha256(payload).hexdigest(),
    }
    meta.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")

    print(json.dumps(metadata, indent=2), flush=True)

    text = payload.decode("utf-8", errors="replace")
    print(text if len(payload) < 20000 else text[-3500:], flush=True)

    return status


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
