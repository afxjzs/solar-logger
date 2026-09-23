#!/usr/bin/env python3

"""
Build the VS Code / cpptools compilation database for every project source file
in the Arduino sketch: the .ino, and each .c/.cpp module beside it.

WHY THIS EXISTS
===============

`arduino-cli compile --only-compilation-database` writes a correct
compile_commands.json, but it describes the files the compiler sees, not the
files a person edits. Four separate things make the raw database unusable for
the editor, and each one is enough on its own to leave red squiggles:

1. NO ENTRY FOR ANY FILE A PERSON EDITS.
   Arduino CLI copies the sketch into <build>/sketch/ and compiles the copies.
   The .ino is concatenated, given `#include <Arduino.h>` and synthesized
   prototypes, and emitted as <build>/sketch/<name>.ino.cpp. Every module
   (ina228.cpp, connection.cpp, ...) is copied to <build>/sketch/ as it is.
   The database names those copies. cpptools matches entries by path, so it
   finds no configuration for Arduino/solar-logger/ina228.cpp and falls back
   to its defaults: "cannot open source file Arduino.h". The .ino got an entry
   of its own on 2026-09-16; modules had none until 2026-09-18.

2. THE ESP-IDF INCLUDE PATH IS HIDDEN BEHIND TWO LEVELS OF INDIRECTION.
   The Arduino library paths (Wire, WiFi, LittleFS, ...) arrive as plain `-I`
   flags, but every ESP-IDF path arrives as

       -iprefix <sdk>/include/  @<sdk>/flags/includes

   where that response file holds 325 `-iwithprefixbefore <relative>` entries.
   Anything that does not follow both the `@response` file and the `-iprefix`
   chain resolves `Wire.h` and fails on `soc/soc_caps.h`,
   `freertos/FreeRTOS.h`, `esp_sleep.h`, and `esp_rom_crc.h`. This script
   resolves both at generation time, so every editor entry carries plain
   absolute `-I` flags and has nothing left to follow.

3. THE .ino IS NOT AUTOMATICALLY A VALID C++ TRANSLATION UNIT.
   Arduino CLI synthesizes forward prototypes into the generated .ino.cpp, so
   a sketch may call a function defined further down without declaring it. Fed
   the raw .ino, a compiler reports every one of those as undeclared, and no
   include-path configuration fixes it because the declarations really are
   absent.

   The fix for that is in the sketch, not here: solar-logger.ino declares its
   own forward references. This script's job is to notice when that stops being
   true - see VERIFICATION below. A .cpp module has no such gap: nothing is
   synthesized for it, so it must already be valid C++ and it is compiled as it
   is, without the `-include Arduino.h` the .ino gets.

   It matters more than it looks: cpptools defaults `C_Cpp.errorSquiggles` to
   `enabledIfIncludesResolve`, so while the includes were broken it suppressed
   every other error. Fixing only the includes would have uncovered a second
   wave of errors that had been there the whole time.

4. NOTHING NOTICES WHEN THE DATABASE GOES STALE.
   Add an `#include`, a module, or a function, and the old database no longer
   describes the sketch. `--check` compares a recorded fingerprint of every
   sketch source and says so instead of letting the editor drift.

MODULE DISCOVERY
================

Nothing here names a module. The project's translation units are read out of
the database Arduino CLI has just written: every entry for a file under
<build>/sketch/ is one, and it is mapped back to the file a person edits. Add
foo.cpp, run tools/check.sh (which runs this), and foo.cpp has an entry.

The sketch directory is also scanned on its own, where Arduino CLI looks for
sources: the sketch root, and src/ recursively. A C or C++ source found there
that Arduino CLI did not compile fails the run, because the editor would have
no configuration for it. Observed with arduino-cli 1.5.1 on 2026-09-18: .c,
.cpp, .cc, .cxx and .S files in those places are compiled, and a .cpp in any
other subdirectory is copied into the build but not compiled.

Each editor entry REPLACES Arduino CLI's entry for the copy, so every project
translation unit has exactly one entry, and it names the real file. A deleted
module leaves nothing behind: Arduino CLI drops its entry (observed), and the
validation below rejects any entry for a project file that no longer exists.

WHAT THIS DOES NOT DO
=====================

It hardcodes nothing about the toolchain. Every path, define, and flag comes
from the database Arduino CLI just produced; this script only resolves
indirection that Arduino CLI expects a compiler to resolve. Change boards,
cores, or libraries and the next run follows.

It gives assembler (.S) sources no entry, and says so when it finds one:
cpptools offers no IntelliSense for assembly, and `-fsyntax-only` is not a
check for it. The sketch has none today.

VERIFICATION
============

Nothing is written until every project translation unit has been compiled with
`-fsyntax-only`, using exactly the flags its editor entry carries. If any one
fails, the compiler's diagnostics are printed and the previous database stays
in place. Each entry is also checked for the headers the 2026-09-16 failure was
about: Arduino.h, and four ESP-IDF headers reachable only through the -iprefix
chain, must resolve in its include path.

That check is also what keeps point 3 honest: add a call to a function defined
further down without declaring it and this fails by name, here, rather than
turning up later as a squiggle nobody can explain.

Arduino CLI's own database is written into a subdirectory and never over the
file the editor reads. Until 2026-09-18 they were the same file, so a failed
verification left the editor with Arduino CLI's raw database - no entry for the
.ino at all - while this script printed that the previous configuration was
still in place.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import NoReturn, Optional

DATABASE_NAME = "compile_commands.json"
STATUS_NAME = "status.json"

# Arduino CLI's build path, inside the output directory. The database it writes
# there is this script's input and is never the file the editor reads.
ARDUINO_BUILD_DIRNAME = "arduino-cli"

# What Arduino CLI compiles as C or C++ when it sits in the sketch root or under
# src/. Observed with arduino-cli 1.5.1 on 2026-09-18. Each gets an editor entry.
EDITOR_SOURCE_SUFFIXES = (".c", ".cpp", ".cc", ".cxx")

# Also compiled by Arduino CLI, and deliberately given no editor entry. See
# WHAT THIS DOES NOT DO.
ASSEMBLER_SUFFIXES = (".S",)

HEADER_SUFFIXES = (".h", ".hh", ".hpp", ".hxx")

# Must resolve in every project entry's include path. Arduino.h is the core.
# The other four are the ESP-IDF headers that failed on 2026-09-16, all of them
# reachable only through the -iprefix / response-file chain (D-039).
REQUIRED_HEADERS = (
    "Arduino.h",
    "freertos/FreeRTOS.h",
    "esp_sleep.h",
    "soc/soc_caps.h",
    "esp_rom_crc.h",
)

# The ESP-IDF path alone is over 300 entries. Fewer than this means the
# response files were not followed and the editor would fail on exactly the
# headers this script exists to fix.
MINIMUM_INCLUDE_PATHS = 100

INCLUDE_FLAGS = ("-I", "-idirafter", "-isystem")

_INCLUDE_DIRECTIVE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)


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


def sketch_ino(sketch_dir: Path) -> Path:
    """The one .ino Arduino CLI compiles.

    Arduino CLI puts the .ino named after the sketch directory first and then
    the rest alphabetically, concatenating them into one translation unit. This
    project has exactly one. A second one would need its own idea of what "this
    file compiles on its own" means, so it is refused rather than silently left
    unconfigured.
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

    return primary


def sketch_tree(sketch_dir: Path, suffixes: tuple[str, ...]) -> list[Path]:
    """Files with these suffixes where Arduino CLI looks for sketch sources:
    the sketch root, and src/ recursively. Any other subdirectory is not
    compiled, so it is not looked at."""

    found = [p for p in sketch_dir.iterdir() if p.is_file() and p.suffix in suffixes]
    source_dir = sketch_dir / "src"

    if source_dir.is_dir():
        found.extend(p for p in source_dir.rglob("*") if p.is_file() and p.suffix in suffixes)

    return sorted(found)


def editor_sources(sketch_dir: Path) -> list[Path]:
    """Every file that must have an editor entry, found by scanning the sketch
    directory rather than by asking Arduino CLI."""

    return sorted([sketch_ino(sketch_dir), *sketch_tree(sketch_dir, EDITOR_SOURCE_SUFFIXES)])


def fingerprinted_sources(sketch_dir: Path) -> list[Path]:
    """Everything whose change can make the database stale: every source and
    header Arduino CLI would see. firmware_version.h counts, because it changes
    what the image reports about itself."""

    suffixes = EDITOR_SOURCE_SUFFIXES + ASSEMBLER_SUFFIXES + HEADER_SUFFIXES
    return sorted([sketch_ino(sketch_dir), *sketch_tree(sketch_dir, suffixes)])


def fingerprint(sketch_dir: Path, paths: list[Path]) -> str:
    """Content hash of the sketch sources. Relative paths are hashed too, so a
    module that is added, deleted, or moved into src/ changes it."""

    digest = hashlib.sha256()

    for path in sorted(paths):
        digest.update(path.relative_to(sketch_dir).as_posix().encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")

    return digest.hexdigest()


def shown(path: Path, sketch_dir: Path) -> str:
    """A project path as a person would name it."""

    try:
        return path.relative_to(sketch_dir).as_posix()
    except ValueError:
        return str(path)


def direct_includes(source: Path) -> list[str]:
    """The headers a file includes itself, for the verification report only."""

    return _INCLUDE_DIRECTIVE.findall(source.read_text(encoding="utf-8", errors="replace"))


# ============================================================================
# ARDUINO CLI'S DATABASE
# ============================================================================


def generate_arduino_database(fqbn: str, sketch_dir: Path, arduino_build: Path) -> None:
    arduino_build.mkdir(parents=True, exist_ok=True)

    log("Generating Arduino CLI compilation database...")
    log(f"Target: {fqbn}")
    log(f"Arduino CLI build path: {arduino_build}")

    command = [
        "arduino-cli",
        "compile",
        "--only-compilation-database",
        "--fqbn",
        fqbn,
        "--build-path",
        str(arduino_build),
        str(sketch_dir),
    ]

    try:
        # Output is not captured: whatever Arduino CLI says goes straight to the
        # person running this, including its own error text.
        result = subprocess.run(command, check=False)
    except FileNotFoundError:
        fail("arduino-cli is not on PATH. The editor database was NOT touched.")

    if result.returncode != 0:
        fail(
            f"arduino-cli exited {result.returncode} (output above). "
            "The editor database was NOT touched."
        )


def read_database(database_path: Path) -> list[dict]:
    if not database_path.is_file():
        fail(f"Arduino CLI did not create {database_path}")

    try:
        entries = json.loads(database_path.read_text(encoding="utf-8"))

    except json.JSONDecodeError as error:
        fail(f"{database_path} is not valid JSON: {error}")

    if not isinstance(entries, list) or not entries:
        fail(f"{database_path} holds no compile commands")

    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("file"), str):
            fail(f"{database_path} holds an entry with no 'file': {entry!r}")

    return entries


@dataclass(frozen=True)
class Unit:
    """One project translation unit: the file a person edits, and Arduino CLI's
    compile command for its copy."""

    source: Path
    entry: dict
    is_sketch: bool


def project_units(entries: list[dict], arduino_build: Path, sketch_dir: Path) -> list[Unit]:
    """Every entry for a copy under <build>/sketch/, mapped back to its source.

    Matched by path, never by position. The first version of this tooling
    cloned entry [0] and relied on Arduino CLI always emitting the sketch
    first; that is an assumption about output ordering, not a fact about the
    database.
    """

    copies = (arduino_build / "sketch").resolve()
    ino = sketch_ino(sketch_dir)
    units: dict[Path, Unit] = {}

    for entry in entries:
        copy = Path(entry["file"]).resolve()

        try:
            relative = copy.relative_to(copies)
        except ValueError:
            continue  # the core or a library, not a project file

        is_sketch = relative == Path(f"{ino.name}.cpp")
        source = ino if is_sketch else sketch_dir / relative

        if not is_sketch and source.suffix not in EDITOR_SOURCE_SUFFIXES + ASSEMBLER_SUFFIXES:
            fail(
                f"Arduino CLI compiled {copy}, and this script does not know how to "
                f"configure a {source.suffix} file for the editor. Extend "
                "EDITOR_SOURCE_SUFFIXES or ASSEMBLER_SUFFIXES deliberately."
            )

        if source in units:
            fail(f"More than one compile command maps to {source}; refusing to pick one.")

        if not source.is_file():
            fail(
                f"Arduino CLI's database names {copy}, a copy of {source}, which does "
                "not exist. Its database is stale; nothing was written."
            )

        if not isinstance(entry.get("arguments"), list):
            fail(
                f"The compile command for {copy} has no 'arguments' array. This script "
                "does not parse the shell-quoted 'command' form."
            )

        units[source] = Unit(source, entry, is_sketch)

    if ino not in units:
        fail(
            f"No compile command for the copy of {ino.name} under {copies}. Arduino CLI "
            "did not preprocess the sketch into the expected path, so there is nothing "
            "to base the editor configuration on."
        )

    return [units[source] for source in sorted(units)]


# ============================================================================
# COMPILE-COMMAND EXPANSION
# ============================================================================


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


def include_dirs(arguments: list[str]) -> list[str]:
    """Every include directory the arguments name, attached or separate."""

    found: list[str] = []
    index = 0

    while index < len(arguments):
        argument = arguments[index]

        for flag in INCLUDE_FLAGS:
            if argument == flag and index + 1 < len(arguments):
                found.append(arguments[index + 1])
                index += 1
                break

            if argument.startswith(flag) and len(argument) > len(flag):
                found.append(argument[len(flag):])
                break

        index += 1

    return found


# ============================================================================
# EDITOR ENTRIES
# ============================================================================


def editor_entry(unit: Unit, sketch_dir: Path) -> dict:
    arguments = expand_response_files(list(unit.entry["arguments"]))
    arguments = resolve_include_prefixes(arguments)
    arguments = strip_output_arguments(arguments, Path(unit.entry["file"]))

    if not arguments:
        fail(f"Nothing survived argument expansion for {unit.source}.")

    if unit.is_sketch:
        # -x c++ is required: the compiler does not recognize the .ino suffix.
        #
        # -include Arduino.h mirrors the line Arduino CLI prepends to the
        # generated .ino.cpp. It is the one thing the sketch genuinely relies on
        # the build system to supply. A .cpp module is not given it: a module
        # includes what it uses (D-047), and forcing it here would hide a
        # module that forgot.
        arguments += ["-x", "c++", "-include", "Arduino.h"]

    arguments.append(str(unit.source))

    # The sketch directory rather than Arduino CLI's working directory, which is
    # wherever it happened to be run from. Every path is absolute, so this only
    # has to exist; the verification compile runs from it.
    return {
        "directory": str(sketch_dir),
        "arguments": arguments,
        "file": str(unit.source),
    }


def entry_problems(entry: dict, source: Path, sketch_dir: Path) -> list[str]:
    """What is wrong with one project entry, without compiling anything."""

    name = shown(source, sketch_dir)
    arguments = entry.get("arguments")

    if (
        not isinstance(arguments, list)
        or not arguments
        or not all(isinstance(argument, str) for argument in arguments)
    ):
        return [f"{name}: the entry has no argument list"]

    problems: list[str] = []
    directory = entry.get("directory")

    if not isinstance(directory, str) or not Path(directory).is_dir():
        problems.append(f"{name}: working directory {directory!r} does not exist")

    if not Path(arguments[0]).is_file():
        problems.append(f"{name}: compiler {arguments[0]} does not exist")

    if arguments[-1] != str(source):
        problems.append(f"{name}: the compile command does not end with the file it is for")

    leftover = [
        argument
        for argument in arguments
        if argument.startswith(("@", "-iprefix", "-iwithprefix"))
    ]

    if leftover:
        problems.append(
            f"{name}: indirection the editor does not follow remains: {', '.join(leftover[:3])}"
        )

    directories = include_dirs(arguments)

    if len(directories) < MINIMUM_INCLUDE_PATHS:
        problems.append(
            f"{name}: only {len(directories)} include paths; the ESP-IDF include set is missing"
        )

    for header in REQUIRED_HEADERS:
        if not any(os.path.isfile(os.path.join(d, header)) for d in directories):
            problems.append(f"{name}: {header} does not resolve in its include path")

    return problems


def database_problems(entries: object, sketch_dir: Path, arduino_build: Path) -> list[str]:
    """Everything that makes a database unfit for the editor, as a list.

    Checked against the sketch directory as it is now, so an added module with
    no entry and a deleted one whose entry survived are both reported.
    """

    if not isinstance(entries, list) or not entries:
        return ["the database holds no compile commands"]

    expected = set(editor_sources(sketch_dir))
    copies = (arduino_build / "sketch").resolve()
    seen: dict[Path, int] = {}
    problems: list[str] = []

    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("file"), str):
            problems.append(f"an entry has no 'file': {entry!r}")
            continue

        path = Path(entry["file"])

        if path.resolve().is_relative_to(copies):
            problems.append(
                f"{path}: an entry for Arduino CLI's build copy survived; the editor "
                "entry for the real file should have replaced it"
            )
            continue

        if not path.is_relative_to(sketch_dir):
            continue  # the core or a library, exactly as Arduino CLI wrote it

        seen[path] = seen.get(path, 0) + 1

        if path not in expected:
            problems.append(
                f"{shown(path, sketch_dir)}: STALE entry. This is not a current project "
                "source: it was deleted, or Arduino CLI does not compile it."
            )
            continue

        problems.extend(entry_problems(entry, path, sketch_dir))

    for source in sorted(expected):
        count = seen.get(source, 0)

        if count == 0:
            problems.append(
                f"{shown(source, sketch_dir)}: NO entry. The editor has no configuration "
                "for this file."
            )
        elif count > 1:
            problems.append(f"{shown(source, sketch_dir)}: {count} entries; expected one")

    return problems


def verify_compiles(entries: list[dict], sketch_dir: Path) -> None:
    """Compile every project file with exactly its editor flags before shipping them."""

    log(
        f"Verifying: compiling all {len(entries)} project translation units with exactly "
        "their editor flags..."
    )

    def compile_one(entry: dict) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [*entry["arguments"], "-fsyntax-only"],
            cwd=entry["directory"],
            capture_output=True,
            text=True,
            check=False,
        )

    workers = max(1, min(len(entries), os.cpu_count() or 1))

    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        results = list(pool.map(compile_one, entries))

    failed: list[str] = []

    for entry, result in zip(entries, results):
        source = Path(entry["file"])
        name = shown(source, sketch_dir)

        if result.returncode == 0:
            includes = ", ".join(direct_includes(source)) or "none"
            log(f"  {name}: compiles cleanly: ALL OK (direct includes: {includes})")
            continue

        failed.append(name)
        sys.stderr.write(f"[INTELLISENSE] --- {name}: exit {result.returncode} ---\n")
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)

    if failed:
        fail(
            f"{', '.join(failed)} did NOT compile under the flags the editor is about to be "
            "given (compiler output above). The database has NOT been written, so the "
            "previous one is still in place."
        )


# ============================================================================
# WRITING
# ============================================================================


def write_json(path: Path, value: object) -> None:
    """Replace a file atomically: a reader sees the old file or the new one,
    never a half-written one."""

    temporary_path = path.with_name(path.name + ".tmp")
    temporary_path.write_text(json.dumps(value, indent=1) + "\n", encoding="utf-8")

    # Readable by the editor. mktemp-style creation would leave it 0600, which
    # is the sort of detail that makes a tool work for one user and not another.
    temporary_path.chmod(0o644)
    os.replace(temporary_path, path)


def report_problems(problems: list[str]) -> None:
    for problem in problems:
        log(f"  PROBLEM: {problem}")


# ============================================================================
# GENERATE / CHECK
# ============================================================================


def generate(
    sketch_dir: Path,
    output_dir: Path,
    arduino_build: Path,
    fqbn: str,
    run_arduino_cli: bool = True,
) -> int:
    """Regenerate and verify the editor database. `run_arduino_cli=False`
    reuses the database already in `arduino_build`; the tests use it to reach
    the verification failure path without a second Arduino CLI run."""

    if run_arduino_cli:
        generate_arduino_database(fqbn, sketch_dir, arduino_build)

    entries = read_database(arduino_build / DATABASE_NAME)
    units = project_units(entries, arduino_build, sketch_dir)

    # The independent cross-check: scanned from the sketch directory, not read
    # from Arduino CLI's output.
    compiled = {unit.source for unit in units}
    not_compiled = [path for path in editor_sources(sketch_dir) if path not in compiled]

    if not_compiled:
        fail(
            "Arduino CLI did not compile "
            + ", ".join(shown(path, sketch_dir) for path in not_compiled)
            + ", so the editor would have no configuration for it. Nothing was written."
        )

    editor_units = [unit for unit in units if unit.source.suffix not in ASSEMBLER_SUFFIXES]

    log(f"Project translation units found ({len(editor_units)}):")

    for unit in editor_units:
        log(f"  {shown(unit.source, sketch_dir)}")

    for unit in units:
        if unit.source.suffix in ASSEMBLER_SUFFIXES:
            log(
                f"  NOTE: {shown(unit.source, sketch_dir)} is assembler and gets NO editor "
                "entry; cpptools offers no IntelliSense for assembly."
            )

    editor_entries = [editor_entry(unit, sketch_dir) for unit in editor_units]

    include_counts = sorted({len(include_dirs(e["arguments"])) for e in editor_entries})
    log(f"Expanded each compile command to {', '.join(map(str, include_counts))} absolute include paths")
    log("No response file or -iprefix indirection remains in any project entry")

    # Every project unit's copy entry is replaced by its editor entry. The core
    # and library entries are kept exactly as Arduino CLI wrote them.
    replaced = {Path(unit.entry["file"]).resolve() for unit in units}
    kept = [entry for entry in entries if Path(entry["file"]).resolve() not in replaced]
    combined = [*kept, *editor_entries]

    problems = database_problems(combined, sketch_dir, arduino_build)

    if problems:
        report_problems(problems)
        fail(
            f"The new database has {len(problems)} problem(s), listed above. It has NOT "
            "been written, so the previous one is still in place."
        )

    log(
        f"Every project entry resolves {', '.join(REQUIRED_HEADERS)}: ALL OK"
    )

    verify_compiles(editor_entries, sketch_dir)

    output_dir.mkdir(parents=True, exist_ok=True)
    database_path = output_dir / DATABASE_NAME
    write_json(database_path, combined)

    written = json.loads(database_path.read_text(encoding="utf-8"))

    if written != combined:  # pragma: no cover - a filesystem that lies
        fail(f"{database_path} did not read back as what was written.")

    write_json(
        output_dir / STATUS_NAME,
        {
            "sketch_dir": str(sketch_dir),
            "fingerprint": fingerprint(sketch_dir, fingerprinted_sources(sketch_dir)),
            "translation_units": [shown(unit.source, sketch_dir) for unit in editor_units],
        },
    )

    log(f"Compilation database ready: {database_path}")
    log(
        f"Entries: {len(combined)}, of which {len(editor_entries)} name project files "
        "and the rest are the core and libraries as Arduino CLI wrote them"
    )
    return 0


def check(sketch_dir: Path, output_dir: Path, arduino_build: Path) -> int:
    """Report whether the database still describes the sketch. Changes nothing
    and compiles nothing."""

    status_path = output_dir / STATUS_NAME
    database_path = output_dir / DATABASE_NAME

    if not status_path.is_file() or not database_path.is_file():
        log(f"NOT ALL OK - no database has been generated in {output_dir} yet.")
        log("Run: tools/intellisense.sh")
        return 1

    try:
        status = json.loads(status_path.read_text(encoding="utf-8"))
        entries = json.loads(database_path.read_text(encoding="utf-8"))

    except json.JSONDecodeError as error:
        log(f"NOT ALL OK - the database or its status file is not valid JSON: {error}")
        log("Run: tools/intellisense.sh")
        return 1

    problems = database_problems(entries, sketch_dir, arduino_build)
    current = fingerprint(sketch_dir, fingerprinted_sources(sketch_dir))
    stale = not isinstance(status, dict) or status.get("fingerprint") != current

    if not problems and not stale:
        units = editor_sources(sketch_dir)
        log(
            f"Compilation database matches the sketch and covers all {len(units)} project "
            "translation units: ALL OK"
        )

        for unit in units:
            log(f"  {shown(unit, sketch_dir)}")

        return 0

    report_problems(problems)

    if stale:
        log("The sketch has changed since the database was generated.")

    log("NOT ALL OK - the editor configuration does not describe the current sketch.")
    log("Run: tools/intellisense.sh")
    return 1


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="intellisense.py",
        description="Build a VS Code cpptools compilation database for every project file.",
    )
    parser.add_argument("--sketch", required=True, type=Path)
    parser.add_argument(
        "--output-dir",
        required=True,
        type=Path,
        help="where the editor database and its status file are written",
    )
    parser.add_argument(
        "--arduino-build-path",
        type=Path,
        help=f"Arduino CLI's build path (default: <output-dir>/{ARDUINO_BUILD_DIRNAME})",
    )
    parser.add_argument("--fqbn", help="the board to generate for; required unless --check")
    parser.add_argument(
        "--check",
        action="store_true",
        help="report whether the database still matches the sketch, and change nothing",
    )

    args = parser.parse_args(argv)

    sketch_dir = args.sketch.resolve()
    output_dir = args.output_dir.resolve()
    arduino_build = (args.arduino_build_path or output_dir / ARDUINO_BUILD_DIRNAME).resolve()

    if not sketch_dir.is_dir():
        log(f"ERROR: {sketch_dir} is not a directory.")
        return 1

    try:
        if args.check:
            return check(sketch_dir, output_dir, arduino_build)

        if not args.fqbn:
            log("ERROR: --fqbn is required to generate the database.")
            return 1

        return generate(sketch_dir, output_dir, arduino_build, args.fqbn)

    except GeneratorError as error:
        log(f"ERROR: {error}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
