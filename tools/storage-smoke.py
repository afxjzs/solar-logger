"""Drive one healthy-log storage acceptance, stage by stage.

    tools/storage-smoke.py preflight --evidence-dir DIR --revision REV
    tools/storage-smoke.py identify  --evidence-dir DIR --revision REV
    tools/storage-smoke.py growth    --evidence-dir DIR --revision REV
    tools/storage-smoke.py session   --evidence-dir DIR --revision REV

Each stage captures its commands through tools/capture-command.py and asserts
its own result, so a stage that fails stops there rather than letting later
stages run against a board in an unexpected state. `--revision` is the Git
revision the board must report for that stage: the OLD image for `preflight`,
the newly uploaded one for everything after it.

This drives the HEALTHY-log path only, which is the path a storage change must
not have moved. It does not exercise a damaged log, a failed open or a short
read. Those need a real filesystem fault, and one must never be obtained by
damaging a live experiment's log.

THE NEGATIVE CRITERION IS THE POINT. The storage recovery changes of 2026-09-29
added operator messages that only unhealthy paths can print. On a healthy log
none of them may appear, so their absence is asserted on every single capture
rather than checked once at the end. A healthy log that prints one of them
means the healthy path moved, and acceptance stops.

The dumps are deliberately NOT driven from here. They are long captures whose
output is the evidence, and they are run and validated as their own step.
"""

import argparse
import pathlib
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
CAPTURE = REPO / "tools" / "capture-command.py"

FORBIDDEN = (
    "UNREADABLE",
    "could not be opened",
    "Running totals could NOT be recovered",
    "Carrying the retained totals instead",
    "Retained RTC state was",
    "Restarting the running totals",
    "was never read",
    "[STORAGE] ERROR",
    "[STORAGE] WARNING",
    "NOT ALL OK",
    "TRUNCATED",
)

# A sleeping board's USB device is absent until the next rendezvous, so a
# command that starts one waits. KEEPALIVE, RELEASE and a held STATUS address a
# session that already exists and must NOT wait: tools/send.sh refuses --wait
# on those rather than ignoring it.
WAIT = ["--wait", "--timeout", "150"]
WAIT_LONG = ["--wait", "--timeout", "150", "--capture-seconds", "10"]
HELD: list[str] = []


class StageFailed(Exception):
    pass


def run(evidence: pathlib.Path, label: str, options: list[str],
        command: str) -> str:
    argv = [sys.executable, str(CAPTURE), str(evidence), label,
            "tools/send.sh", *options, *command.split()]
    subprocess.run(argv, cwd=REPO, check=True)

    text = (evidence / f"{label}.txt").read_text(encoding="utf-8")

    if f"Firmware parsed command: ALL OK (CMD_ACK,{command})" not in text:
        raise StageFailed(f"{label}: no CMD_ACK matching {command!r}. A queued "
                          f"command is not a completed one.")

    if f"CMD_RESULT,{command},OK" not in text:
        raise StageFailed(f"{label}: no CMD_RESULT OK matching {command!r}")

    hits = [x for x in FORBIDDEN if x in text]
    if hits:
        raise StageFailed(f"{label}: STOP ACCEPTANCE. A healthy log printed "
                          f"{hits}, which only an unhealthy path should print.")

    return text


def expect(text: str, needle: str, why: str) -> None:
    if needle not in text:
        raise StageFailed(f"{why} (expected {needle!r})")


def healthy_log(text: str) -> None:
    expect(text, "Tail status:      INTACT", "the tail is not reported INTACT")
    expect(text, "Trailing bytes:   0", "trailing bytes are not zero")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage",
                        choices=("preflight", "identify", "growth", "session"))
    parser.add_argument("--evidence-dir", type=pathlib.Path, required=True)
    parser.add_argument("--revision", default=None,
                        help="Git revision the board must report. Required by "
                             "the stages that capture VERSION (preflight, "
                             "identify) and not accepted by the others, which "
                             "do not check it.")
    parser.add_argument("--experiment", type=int, default=3)
    options = parser.parse_args()

    # Asked for only where it is actually verified. A flag that is accepted and
    # ignored reads like a check that happened.
    checks_revision = options.stage in ("preflight", "identify")

    if checks_revision and options.revision is None:
        parser.error(f"--revision is required for the {options.stage} stage")

    if not checks_revision and options.revision is not None:
        parser.error(f"the {options.stage} stage captures no VERSION, so it "
                     f"cannot check --revision; omit it")

    evidence: pathlib.Path = options.evidence_dir
    evidence.mkdir(parents=True, exist_ok=True)

    prefix = "pre" if options.stage == "preflight" else "post"

    try:
        if options.stage == "preflight":
            text = run(evidence, "pre-version", WAIT, "VERSION")
            expect(text, f"[FIRMWARE] Revision: {options.revision}\n",
                   "the board is not running the revision this stage expects; "
                   "the preflight dump would not describe the pre-change log")
            run(evidence, "pre-status", WAIT, "STATUS")
            text = run(evidence, "pre-storage-info", WAIT_LONG,
                       "LOGGER STORAGE INFO")
            healthy_log(text)
            text = run(evidence, "pre-autonomous-status", WAIT,
                       "LOGGER AUTONOMOUS STATUS")
            expect(text, "[AUTO] Armed: YES", "the test is not armed")
            expect(text,
                   f"[AUTO] New records will carry experiment id: {options.experiment}",
                   f"new records would not carry experiment {options.experiment}")

        elif options.stage == "identify":
            text = run(evidence, "post-version", WAIT, "VERSION")
            expect(text, f"[FIRMWARE] Revision: {options.revision}\n",
                   "the board is NOT running the uploaded revision")
            text = run(evidence, "post-storage-info", WAIT_LONG,
                       "LOGGER STORAGE INFO")
            healthy_log(text)
            text = run(evidence, "post-autonomous-status", WAIT,
                       "LOGGER AUTONOMOUS STATUS")
            expect(text, "[AUTO] Armed: YES", "the test is not armed")
            expect(text,
                   f"[AUTO] New records will carry experiment id: {options.experiment}",
                   f"new records would not carry experiment {options.experiment}")

        elif options.stage == "growth":
            text = run(evidence, "post-growth-info", WAIT_LONG,
                       "LOGGER STORAGE INFO")
            healthy_log(text)

        elif options.stage == "session":
            run(evidence, "post-hold", WAIT, "LOGGER SESSION HOLD")
            run(evidence, "post-keepalive", HELD, "LOGGER SESSION KEEPALIVE")
            text = run(evidence, "post-held-status", HELD, "STATUS")
            expect(text,
                   f"[STATUS] Experiment ID      = {options.experiment}",
                   "the held STATUS did not restore the experiment checkpoint")
            run(evidence, "post-session-status", HELD, "LOGGER SESSION STATUS")
            run(evidence, "post-release", HELD, "LOGGER SESSION RELEASE")

    except StageFailed as failure:
        print(f"[SMOKE] {options.stage}: FAILED", file=sys.stderr)
        print(f"[SMOKE] {failure}", file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as failure:
        print(f"[SMOKE] {options.stage}: a capture exited {failure.returncode}; "
              f"nothing beyond it was run.", file=sys.stderr)
        return failure.returncode

    print(f"[SMOKE] {options.stage}: ALL OK ({prefix} captures written to "
          f"{evidence})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
