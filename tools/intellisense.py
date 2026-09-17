#!/usr/bin/env python3

"""
Turn Arduino CLI's compilation database into one VS Code / cpptools can use for
the .ino file that is actually open in the editor.

WHY THIS EXISTS
===============

`arduino-cli compile --only-compilation-database` writes a correct
compile_commands.json, but it describes the translation unit the compiler sees,
not the file a person edits. Four separate things make the raw database
unusable for the editor, and each one is enough on its own to leave red
squiggles in the sketch:

1. NO ENTRY FOR THE .ino.
   Arduino CLI concatenates the sketch's .ino files, prepends
   `#include <Arduino.h>`, synthesizes function prototypes, and emits
   `<build>/sketch/<name>.ino.cpp`. The database names that generated file.
   cpptools matches entries by path, so it finds no configuration at all for
   `solar-logger.ino`.

2. THE ESP-IDF INCLUDE PATH IS HIDDEN BEHIND TWO LEVELS OF INDIRECTION.
   The Arduino library paths (Wire, WiFi, LittleFS, ...) arrive as plain `-I`
   flags, but every ESP-IDF path arrives as

       -iprefix <sdk>/include/  @<sdk>/flags/includes

   where that response file holds 325 `-iwithprefixbefore <relative>` entries.
   Anything that does not follow both the `@response` file and the `-iprefix`
   chain resolves `Wire.h` and fails on `soc/soc_caps.h`,
   `freertos/FreeRTOS.h`, `esp_sleep.h`, and `esp_rom_crc.h` - which is exactly
   the symptom this script was written to end. This script resolves both at
   generation time, so the editor entry carries plain absolute `-I` flags and
   has nothing left to follow.

3. THE .ino IS NOT AUTOMATICALLY A VALID C++ TRANSLATION UNIT.
   Arduino CLI synthesizes forward prototypes into the generated .ino.cpp, so
   a sketch may call a function defined further down without declaring it. Fed
   the raw .ino, a compiler reports every one of those as undeclared, and no
   include-path configuration fixes it because the declarations really are
   absent.

   The fix for that is in the sketch, not here: solar-logger.ino declares its
   own forward references. This script's job is to notice when that stops being
   true - see VERIFICATION below.

   It matters more than it looks: cpptools defaults `C_Cpp.errorSquiggles` to
   `enabledIfIncludesResolve`, so while the includes were broken it suppressed
   every other error. Fixing only the includes would have uncovered a second
   wave of errors that had been there the whole time.

4. NOTHING NOTICES WHEN THE DATABASE GOES STALE.
   Add an `#include` to the sketch and Arduino CLI's library detection changes
   the `-I` set, which the old database does not carry. `--check` compares a
   recorded fingerprint and says so instead of letting the editor drift.

WHAT THIS DOES NOT DO
=====================

It hardcodes nothing. Every path, define, and flag comes from the database
Arduino CLI just produced; this script only resolves indirection that Arduino
CLI expects a compiler to resolve. Change boards, cores, or libraries and the
next run follows.

VERIFICATION
============

Before writing anything, the editor command is run for real against the raw
.ino with `-fsyntax-only`. If the sketch does not compile under exactly the
flags the editor is about to be handed, this script fails loudly and leaves the
previous database alone. "IntelliSense should work" is not a claim worth making
without checking it.

That check is also what keeps point 3 honest: add a call to a function defined
further down without declaring it and this fails by name, here, rather than
turning up later as a squiggle nobody can explain.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Iterable, NoReturn, Optional

# Arduino CLI writes this next to the object files.
DATABASE_NAME = "compile_commands.json"

# Everything this script generates goes in one subdirectory of the build path,
# so it is obvious what is Arduino CLI's output and what is ours.
GENERATED_DIRNAME = "intellisense"
STATUS_NAME = "status.json"


class GeneratorError(Exception):
    """A failure the caller must not ignore. Never recovered from silently."""


def log(message: str) -> None:
    print(f"[INTELLISENSE] {message}", flush=True)


def fail(message: str) -> NoReturn:
    """Stop. Declared NoReturn so callers narrow correctly after it."""

    raise GeneratorError(message)


# ============================================================================
# SKETCH SOURCES
# ============================================================================


def sketch_ino_files(sketch_dir: Path) -> list[Path]:
    """The .ino files Arduino CLI will concatenate, in its order.

    Arduino CLI puts the .ino named after the sketch directory first and then
    the rest alphabetically, concatenating them into one translation unit. This
    project has exactly one. A second one would need its own database entry and
    its own idea of what "this file compiles on its own" means, so it is
    refused rather than silently left unconfigured.
    """

    inos = sorted(p for p in sketch_dir.iterdir() if p.suffix == ".ino")

    if not inos:
        fail(f"No .ino file in {sketch_dir}")

    primary = sketch_dir / f"{sketch_dir.name}.ino"

    if primary not in inos:
        fail(f"Expected {primary} to exist; Arduino CLI names the sketch after its directory.")

    if len(inos) > 1:
        fail(
            f"{sketch_dir} holds {len(inos)} .ino files, and this script configures one. "
            "Extend it before adding a second sketch tab - or, better, move the code into "
            "real .h/.cpp modules, which need none of this."
        )

    return [primary]


#: Everything Arduino CLI compiles or copies out of the sketch directory. The
#: fingerprint covers all of it, not just the .ino: firmware_version.h changes
#: what the image reports about itself and would otherwise leave the database
#: looking current.
SKETCH_SOURCE_SUFFIXES = (".ino", ".h", ".hpp", ".c", ".cpp", ".cc", ".S")


def sketch_sources(sketch_dir: Path) -> list[Path]:
    """Every source file in the sketch directory, sorted."""

    return sorted(
        p for p in sketch_dir.iterdir()
        if p.is_file() and p.suffix in SKETCH_SOURCE_SUFFIXES
    )


def fingerprint(paths: Iterable[Path]) -> str:
    """Content hash of the sketch sources, for staleness detection."""

    digest = hashlib.sha256()

    for path in sorted(paths):
        digest.update(path.name.encode("utf-8"))
        digest.update(path.read_bytes())

    return digest.hexdigest()


# ============================================================================
# COMPILE-COMMAND EXPANSION
# ============================================================================


def read_database(build_dir: Path) -> list[dict]:
    database_path = build_dir / DATABASE_NAME

    if not database_path.is_file():
        fail(f"Arduino CLI did not create {database_path}")

    try:
        entries = json.loads(database_path.read_text(encoding="utf-8"))

    except json.JSONDecodeError as error:
        fail(f"{database_path} is not valid JSON: {error}")

    if not isinstance(entries, list) or not entries:
        fail(f"{database_path} holds no compile commands")

    return entries


def find_sketch_entry(entries: list[dict], build_dir: Path, sketch_name: str) -> dict:
    """The entry for the generated <build>/sketch/<name>.ino.cpp.

    Matched by exact path rather than by position. The previous version of this
    tooling cloned entry [0] and relied on Arduino CLI always emitting the
    sketch first; that is an assumption about output ordering, not a fact about
    the database.
    """

    wanted = (build_dir / "sketch" / f"{sketch_name}.ino.cpp").resolve()
    matches = [e for e in entries if Path(e.get("file", "")).resolve() == wanted]

    if not matches:
        fail(
            f"No compile command for {wanted}. Arduino CLI did not preprocess the "
            "sketch into the expected path, so there is nothing to base the editor "
            "configuration on."
        )

    if len(matches) > 1:
        fail(f"{len(matches)} compile commands claim {wanted}; refusing to pick one.")

    entry = matches[0]

    if "arguments" not in entry:
        fail(
            f"The compile command for {wanted} has no 'arguments' array. This script "
            "does not parse the shell-quoted 'command' form."
        )

    return entry


def expand_response_files(arguments: list[str], depth: int = 0) -> list[str]:
    """Replace every @file argument with the arguments it holds.

    GCC response files are how the ESP32 core delivers its defines and its
    entire ESP-IDF include path. A consumer that does not follow them sees a
    fraction of the real compile line.
    """

    if depth > 8:
        fail("Response files nest more than 8 deep; refusing to keep following them.")

    expanded: list[str] = []

    for argument in arguments:
        if not argument.startswith("@"):
            expanded.append(argument)
            continue

        response_path = Path(argument[1:])

        if not response_path.is_file():
            fail(f"Response file {response_path} referenced by the compile command is missing.")

        contents = response_path.read_text(encoding="utf-8")
        expanded.extend(expand_response_files(shlex.split(contents), depth + 1))

    return expanded


def resolve_include_prefixes(arguments: list[str]) -> list[str]:
    """Turn -iprefix / -iwithprefixbefore / -iwithprefix into absolute paths.

    `-iprefix P` sets a prefix; `-iwithprefixbefore X` then means "search P+X
    before the system directories" and `-iwithprefix X` means "search P+X
    after them". Those are ordinary GCC semantics, and resolving them here
    leaves the editor entry with nothing but plain -I and -idirafter flags.
    """

    resolved: list[str] = []
    prefix: Optional[str] = None
    index = 0

    while index < len(arguments):
        argument = arguments[index]

        if argument == "-iprefix":
            if index + 1 >= len(arguments):
                fail("-iprefix is the last argument and has no value.")

            prefix = arguments[index + 1]
            index += 2
            continue

        if argument.startswith("-iprefix") and len(argument) > len("-iprefix"):
            prefix = argument[len("-iprefix"):]
            index += 1
            continue

        if argument in ("-iwithprefixbefore", "-iwithprefix"):
            if index + 1 >= len(arguments):
                fail(f"{argument} is the last argument and has no value.")

            if prefix is None:
                fail(f"{argument} appeared before any -iprefix; the path cannot be resolved.")

            flag = "-I" if argument == "-iwithprefixbefore" else "-idirafter"
            resolved.append(flag + os.path.normpath(os.path.join(prefix, arguments[index + 1])))
            index += 2
            continue

        resolved.append(argument)
        index += 1

    return resolved


def strip_output_arguments(arguments: list[str], source_file: Path) -> list[str]:
    """Drop the parts that only make sense when producing an object file."""

    kept: list[str] = []
    index = 0

    while index < len(arguments):
        argument = arguments[index]

        if argument in ("-MMD", "-MD", "-c"):
            index += 1
            continue

        if argument in ("-o", "-MF"):
            index += 2
            continue

        if Path(argument) == source_file:
            index += 1
            continue

        kept.append(argument)
        index += 1

    return kept


# ============================================================================
# EDITOR ENTRY
# ============================================================================


def build_editor_arguments(entry: dict, sketch_ino: Path) -> list[str]:
    arguments = expand_response_files(list(entry["arguments"]))
    arguments = resolve_include_prefixes(arguments)
    arguments = strip_output_arguments(arguments, Path(entry["file"]))

    if not arguments:
        fail("Nothing survived argument expansion; the compile command was not understood.")

    compiler = arguments[0]
    flags = arguments[1:]

    include_count = len([f for f in flags if f.startswith(("-I", "-idirafter", "-isystem"))])

    if include_count < 100:
        # The ESP-IDF path alone is over 300 entries. A number this small means
        # the response files were not followed and the editor would fail on
        # exactly the headers this script exists to fix.
        fail(
            f"Only {include_count} include paths survived expansion. The ESP-IDF include "
            "set is missing, so the generated configuration would repeat the original bug."
        )

    # -x c++ is required: the compiler does not recognize the .ino suffix.
    #
    # -include Arduino.h mirrors the line Arduino CLI prepends to the generated
    # .ino.cpp. It is the one thing the sketch genuinely relies on the build
    # system to supply, so the editor has to be told about it explicitly.
    return [
        compiler,
        *flags,
        "-x", "c++",
        "-include", "Arduino.h",
        str(sketch_ino),
    ]


def verify_editor_arguments(arguments: list[str]) -> None:
    """Compile the sketch with exactly the editor's flags before shipping them."""

    command = [*arguments, "-fsyntax-only"]

    log("Verifying the editor configuration by compiling the sketch with it...")

    result = subprocess.run(command, capture_output=True, text=True, check=False)

    if result.returncode == 0:
        log("Editor configuration compiles the sketch cleanly: ALL OK")
        return

    sys.stderr.write(result.stdout)
    sys.stderr.write(result.stderr)

    fail(
        "The sketch does NOT compile under the flags the editor is about to be given "
        f"(exit {result.returncode}, output above). The database has NOT been written, so "
        "the previous configuration is still in place."
    )


def write_database(build_dir: Path, entries: list[dict], editor_entry: dict) -> Path:
    """Replace the database atomically, with the editor entry appended."""

    editor_file = Path(editor_entry["file"]).resolve()

    kept = [e for e in entries if Path(e.get("file", "")).resolve() != editor_file]
    combined = [*kept, editor_entry]

    database_path = build_dir / DATABASE_NAME
    temporary_path = database_path.with_suffix(".json.tmp")

    temporary_path.write_text(json.dumps(combined, indent=1) + "\n", encoding="utf-8")

    # Readable by the editor. mktemp-style creation would leave it 0600, which
    # is the sort of detail that makes a tool work for one user and not another.
    temporary_path.chmod(0o644)

    # Atomic within the directory: a reader sees the old file or the new one,
    # never a half-written one.
    os.replace(temporary_path, database_path)

    return database_path


# ============================================================================
# STATUS / STALENESS
# ============================================================================


def status_path(build_dir: Path) -> Path:
    return build_dir / GENERATED_DIRNAME / STATUS_NAME


def write_status(build_dir: Path, sketch_dir: Path, sources: list[Path], includes: int) -> None:
    status_path(build_dir).write_text(
        json.dumps(
            {
                "sketch_dir": str(sketch_dir),
                "sources": [str(p) for p in sources],
                "fingerprint": fingerprint(sources),
                "include_paths": includes,
            },
            indent=1,
        )
        + "\n",
        encoding="utf-8",
    )


def check_status(build_dir: Path, sketch_dir: Path) -> int:
    path = status_path(build_dir)

    if not path.is_file():
        log(f"NOT ALL OK - no database has been generated yet ({path} is missing).")
        log("Run: tools/intellisense.sh")
        return 1

    status = json.loads(path.read_text(encoding="utf-8"))
    current = fingerprint(sketch_sources(sketch_dir))

    if current == status.get("fingerprint"):
        log("Compilation database matches the current sketch: ALL OK")
        return 0

    log("NOT ALL OK - the sketch has changed since the database was generated.")
    log("The recorded include paths may no longer describe this sketch.")
    log("Run: tools/intellisense.sh")
    return 1


# ============================================================================


def generate(build_dir: Path, sketch_dir: Path) -> int:
    sketch_ino = sketch_ino_files(sketch_dir)[0]
    sources = sketch_sources(sketch_dir)

    entries = read_database(build_dir)
    entry = find_sketch_entry(entries, build_dir, sketch_dir.name)

    (build_dir / GENERATED_DIRNAME).mkdir(parents=True, exist_ok=True)

    arguments = build_editor_arguments(entry, sketch_ino)

    include_paths = [a for a in arguments if a.startswith(("-I", "-idirafter", "-isystem"))]
    log(f"Expanded the compile command to {len(include_paths)} absolute include paths")
    log("No response file or -iprefix indirection remains in the editor entry")

    verify_editor_arguments(arguments)

    editor_entry = {
        "directory": entry.get("directory", str(sketch_dir)),
        "arguments": arguments,
        "file": str(sketch_ino),
    }

    database_path = write_database(build_dir, entries, editor_entry)
    write_status(build_dir, sketch_dir, sources, len(include_paths))

    log(f"Compilation database ready: {database_path}")
    log(f"Entries: {len(entries) + 1} (including the editor entry for {sketch_ino.name})")
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        prog="intellisense.py",
        description="Make Arduino CLI's compilation database usable by VS Code cpptools.",
    )
    parser.add_argument("--build-path", required=True, type=Path)
    parser.add_argument("--sketch", required=True, type=Path)
    parser.add_argument(
        "--check",
        action="store_true",
        help="report whether the database still matches the sketch, and change nothing",
    )

    args = parser.parse_args(argv)

    build_dir = args.build_path.resolve()
    sketch_dir = args.sketch.resolve()

    if not sketch_dir.is_dir():
        log(f"ERROR: {sketch_dir} is not a directory.")
        return 1

    try:
        if args.check:
            return check_status(build_dir, sketch_dir)

        return generate(build_dir, sketch_dir)

    except GeneratorError as error:
        log(f"ERROR: {error}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
