"""The editor database: every project module is found, and a bad database is refused.

tools/intellisense.py builds the database cpptools reads. Its job since
2026-09-18 is that a new module needs no configuration: ina228.cpp had no
entry, and the editor could not find Arduino.h in it.

Two kinds of test, deliberately:

  SCENARIO     The real generator, the real Arduino CLI and the real ESP32
               toolchain, run against a scratch copy of the sketch with a probe
               module added, then broken, then deleted. The copy is taken with
               tests/firmware_source.py, so no module is named here either.
               Arduino CLI missing is a failure, not a skip: without it this
               has no executed cover.

  VALIDATOR    database_problems() and friends on hand-built databases, one
               defect at a time, so every refusal is shown to fire. The scenario
               only reaches the success paths and one compile failure.

Neither says anything about what the VS Code extension displays. That is
observed in the editor, not here.
"""

from __future__ import annotations

import json
import shutil
from collections import Counter
from pathlib import Path

import pytest

import firmware_source
import intellisense

FQBN = "esp32:esp32:XIAO_ESP32C3"

PROBE_HEADER = """\
#pragma once

int probeModuleValue();
"""

PROBE_SOURCE = """\
#include "zz_probe_module.h"

#include <Arduino.h>
#include <Wire.h>

int probeModuleValue()
{
\treturn Wire.available();
}
"""

BROKEN_PROBE_SOURCE = """\
#include "zz_probe_module.h"

int probeModuleValue()
{
\treturn probeFunctionNobodyDeclared();
}
"""

NESTED_SOURCE = """\
#include <Arduino.h>

int nestedProbeValue()
{
\treturn static_cast<int>(millis());
}
"""


# ============================================================================
# SCENARIO: the real toolchain, a scratch copy, a module added and removed
# ============================================================================


def project_files_in_database(database: Path, sketch_dir: Path) -> Counter[str]:
    entries = json.loads(database.read_text(encoding="utf-8"))
    return Counter(
        entry["file"] for entry in entries if entry["file"].startswith(f"{sketch_dir}/")
    )


def cpp_sources(sketch_dir: Path) -> set[str]:
    """Found here with a glob, independently of the generator's own scan."""

    found = {sketch_dir / "solar-logger.ino", *sketch_dir.glob("*.cpp")}
    found |= set((sketch_dir / "src").rglob("*.cpp"))
    return {str(path) for path in found}


def test_a_module_is_configured_and_removed_with_no_configuration(tmp_path, capsys):
    if shutil.which("arduino-cli") is None:
        pytest.fail(
            "arduino-cli is not on PATH. This test runs the real generator against a "
            "copy of the sketch; without Arduino CLI it has no executed cover, so this "
            "is a failure, not a skip."
        )

    sketch_dir = (tmp_path / "solar-logger").resolve()

    for path in firmware_source.sketch_files():
        target = sketch_dir / path.relative_to(firmware_source.SKETCH_DIR)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)

    probe = sketch_dir / "zz_probe_module.cpp"
    probe_header = sketch_dir / "zz_probe_module.h"
    nested = sketch_dir / "src" / "probe" / "nested_probe.cpp"
    probe.write_text(PROBE_SOURCE, encoding="utf-8")
    probe_header.write_text(PROBE_HEADER, encoding="utf-8")
    nested.parent.mkdir(parents=True)
    nested.write_text(NESTED_SOURCE, encoding="utf-8")

    output_dir = tmp_path / "intellisense"
    database = output_dir / intellisense.DATABASE_NAME
    generate = ["--sketch", str(sketch_dir), "--output-dir", str(output_dir), "--fqbn", FQBN]
    check = ["--check", "--sketch", str(sketch_dir), "--output-dir", str(output_dir)]

    # 1. ADDED. Every C++ source in the copy has exactly one entry, naming the
    #    real file, and each one compiled with its editor flags.
    assert intellisense.main(generate) == 0
    report = capsys.readouterr().out

    assert project_files_in_database(database, sketch_dir) == {
        source: 1 for source in cpp_sources(sketch_dir)
    }
    assert str(probe) in cpp_sources(sketch_dir) and str(nested) in cpp_sources(sketch_dir)

    assert (
        "zz_probe_module.cpp: compiles cleanly: ALL OK "
        "(direct includes: zz_probe_module.h, Arduino.h, Wire.h)"
    ) in report
    assert "src/probe/nested_probe.cpp: compiles cleanly: ALL OK" in report

    entries = json.loads(database.read_text(encoding="utf-8"))
    assert not [e for e in entries if "/arduino-cli/sketch/" in e["file"]], (
        "an entry still names Arduino CLI's build copy"
    )

    for entry in entries:
        if entry["file"].startswith(f"{sketch_dir}/"):
            assert Path(entry["file"]).is_file()
            assert Path(entry["directory"]).is_dir()
            assert entry["arguments"][-1] == entry["file"]

    assert intellisense.main(check) == 0
    capsys.readouterr()

    # 2. BROKEN. A module that stops compiling is refused, and the database the
    #    editor reads is left exactly as it was. Arduino CLI's database from
    #    step 1 is reused, so only the verification runs.
    #
    #    The refused database would hold the same entries as the previous one,
    #    so "the bytes did not change" would also pass for a rewrite. Trailing
    #    whitespace marks the previous file: still valid JSON, and never what
    #    the generator writes, so a rewrite of any kind shows.
    before = database.read_bytes() + b" \n"
    database.write_bytes(before)
    probe.write_text(BROKEN_PROBE_SOURCE, encoding="utf-8")

    with pytest.raises(intellisense.GeneratorError, match="zz_probe_module.cpp did NOT compile"):
        intellisense.generate(
            sketch_dir,
            output_dir.resolve(),
            (output_dir / intellisense.ARDUINO_BUILD_DIRNAME).resolve(),
            FQBN,
            run_arduino_cli=False,
        )

    assert "probeFunctionNobodyDeclared" in capsys.readouterr().err
    assert database.read_bytes() == before, "a refused database overwrote the previous one"

    assert intellisense.main(check) == 1
    assert "The sketch has changed since the database was generated." in capsys.readouterr().out

    # 3. DELETED. Before regenerating, the check names the stale entry. After,
    #    the deleted module's entry is gone and nothing else changed.
    probe.unlink()
    probe_header.unlink()
    nested.unlink()

    assert intellisense.main(check) == 1
    out = capsys.readouterr().out
    assert "zz_probe_module.cpp: STALE entry" in out
    assert "src/probe/nested_probe.cpp: STALE entry" in out
    assert "Run: tools/intellisense.sh" in out

    assert intellisense.main(generate) == 0
    capsys.readouterr()

    files = project_files_in_database(database, sketch_dir)
    assert str(probe) not in files and str(nested) not in files
    assert files == {source: 1 for source in cpp_sources(sketch_dir)}

    assert intellisense.main(check) == 0


# ============================================================================
# VALIDATOR: hand-built databases, one defect at a time
# ============================================================================


class Toolchain:
    """Files standing in for a compiler and its include path. Nothing runs."""

    def __init__(self, root: Path):
        self.compiler = root / "toolchain" / "bin" / "fake-g++"
        self.compiler.parent.mkdir(parents=True)
        self.compiler.write_text("", encoding="utf-8")

        self.headers = root / "toolchain" / "include"

        for header in intellisense.REQUIRED_HEADERS:
            (self.headers / header).parent.mkdir(parents=True, exist_ok=True)
            (self.headers / header).write_text("", encoding="utf-8")

        # Directories need not exist; only the headers above are looked up.
        self.padding = [
            f"-I{root / 'toolchain' / f'sdk{index}'}"
            for index in range(intellisense.MINIMUM_INCLUDE_PATHS)
        ]

    def entry(self, source: Path, directory: Path) -> dict:
        return {
            "directory": str(directory),
            "arguments": [str(self.compiler), f"-I{self.headers}", *self.padding, str(source)],
            "file": str(source),
        }


@pytest.fixture
def project(tmp_path):
    sketch_dir = tmp_path / "solar-logger"
    (sketch_dir / "src" / "beta").mkdir(parents=True)

    for name in ("solar-logger.ino", "alpha.cpp", "alpha.h", "src/beta/gamma.cpp"):
        (sketch_dir / name).write_text(f"// {name}\n", encoding="utf-8")

    toolchain = Toolchain(tmp_path)
    arduino_build = tmp_path / "out" / "arduino-cli"
    core_entry = {
        "directory": str(tmp_path),
        "arguments": [str(toolchain.compiler), "@/somewhere/flags", "-c", "core.cpp"],
        "file": str(tmp_path / "core" / "core.cpp"),
    }
    entries = [
        core_entry,
        *(
            toolchain.entry(sketch_dir / name, sketch_dir)
            for name in ("solar-logger.ino", "alpha.cpp", "src/beta/gamma.cpp")
        ),
    ]
    return sketch_dir, arduino_build, toolchain, entries


def test_a_complete_database_has_no_problems(project):
    sketch_dir, arduino_build, _, entries = project

    assert intellisense.database_problems(entries, sketch_dir, arduino_build) == []


def _drop(entries, sketch_dir, name):
    return [e for e in entries if e["file"] != str(sketch_dir / name)]


def _replace(entries, sketch_dir, name, **changes):
    return [
        {**e, **changes} if e["file"] == str(sketch_dir / name) else e for e in entries
    ]


def _arguments(entries, sketch_dir, name):
    return next(e["arguments"] for e in entries if e["file"] == str(sketch_dir / name))


DEFECTS = [
    pytest.param(
        lambda s, b, t, e: _drop(e, s, "alpha.cpp"),
        "alpha.cpp: NO entry",
        id="module-with-no-entry",
    ),
    pytest.param(
        lambda s, b, t, e: _drop(e, s, "src/beta/gamma.cpp"),
        "src/beta/gamma.cpp: NO entry",
        id="nested-module-with-no-entry",
    ),
    pytest.param(
        lambda s, b, t, e: [*e, t.entry(s / "deleted.cpp", s)],
        "deleted.cpp: STALE entry",
        id="deleted-module-entry-survives",
    ),
    pytest.param(
        lambda s, b, t, e: [*e, t.entry(b / "sketch" / "alpha.cpp", s)],
        "an entry for Arduino CLI's build copy survived",
        id="build-copy-entry-survives",
    ),
    pytest.param(
        lambda s, b, t, e: [*e, t.entry(s / "alpha.cpp", s)],
        "alpha.cpp: 2 entries",
        id="duplicate-entry",
    ),
    pytest.param(
        lambda s, b, t, e: _replace(
            e, s, "alpha.cpp",
            arguments=[str(t.compiler), "@/sdk/flags/includes", *_arguments(e, s, "alpha.cpp")[1:]],
        ),
        "alpha.cpp: indirection the editor does not follow remains",
        id="response-file-left-in",
    ),
    pytest.param(
        lambda s, b, t, e: _replace(
            e, s, "alpha.cpp",
            arguments=[str(t.compiler), *t.padding, str(s / "alpha.cpp")],
        ),
        "alpha.cpp: Arduino.h does not resolve in its include path",
        id="arduino-h-unresolvable",
    ),
    pytest.param(
        lambda s, b, t, e: _replace(
            e, s, "alpha.cpp",
            arguments=[str(t.compiler), f"-I{t.headers}", str(s / "alpha.cpp")],
        ),
        "alpha.cpp: only 1 include paths; the ESP-IDF include set is missing",
        id="esp-idf-include-set-missing",
    ),
    pytest.param(
        lambda s, b, t, e: _replace(e, s, "alpha.cpp", directory=str(s / "nowhere")),
        "alpha.cpp: working directory",
        id="working-directory-missing",
    ),
    pytest.param(
        lambda s, b, t, e: _replace(
            e, s, "alpha.cpp",
            arguments=["/no/such/compiler", *_arguments(e, s, "alpha.cpp")[1:]],
        ),
        "alpha.cpp: compiler /no/such/compiler does not exist",
        id="compiler-missing",
    ),
    pytest.param(
        lambda s, b, t, e: _replace(
            e, s, "alpha.cpp", arguments=_arguments(e, s, "alpha.cpp")[:-1]
        ),
        "alpha.cpp: the compile command does not end with the file it is for",
        id="source-missing-from-command",
    ),
]


@pytest.mark.parametrize(("mutate", "expected"), DEFECTS)
def test_each_defect_is_reported_by_name(project, mutate, expected):
    sketch_dir, arduino_build, toolchain, entries = project

    problems = intellisense.database_problems(
        mutate(sketch_dir, arduino_build, toolchain, entries), sketch_dir, arduino_build
    )

    assert any(expected in problem for problem in problems), problems


def test_a_module_added_later_is_missing_until_regenerated(project):
    """No list of modules exists anywhere to forget to update: the next scan
    of the sketch directory expects the new file."""

    sketch_dir, arduino_build, _, entries = project
    (sketch_dir / "delta.cc").write_text("", encoding="utf-8")

    assert intellisense.database_problems(entries, sketch_dir, arduino_build) == [
        "delta.cc: NO entry. The editor has no configuration for this file."
    ]


def copy_entry(arduino_build: Path, relative: str) -> dict:
    copy = arduino_build / "sketch" / relative
    return {"directory": "/", "arguments": ["g++", "-c", str(copy)], "file": str(copy)}


def test_build_copies_map_back_to_the_files_a_person_edits(project):
    sketch_dir, arduino_build, _, _ = project
    entries = [
        {"directory": "/", "arguments": ["g++"], "file": "/core/core.cpp"},
        copy_entry(arduino_build, "solar-logger.ino.cpp"),
        copy_entry(arduino_build, "alpha.cpp"),
        copy_entry(arduino_build, "src/beta/gamma.cpp"),
    ]

    units = intellisense.project_units(entries, arduino_build, sketch_dir)

    assert [(u.source, u.is_sketch) for u in units] == [
        (sketch_dir / "alpha.cpp", False),
        (sketch_dir / "solar-logger.ino", True),
        (sketch_dir / "src" / "beta" / "gamma.cpp", False),
    ]


@pytest.mark.parametrize(
    ("relatives", "message"),
    [
        pytest.param(
            ["solar-logger.ino.cpp", "gone.cpp"],
            "which does not exist",
            id="copy-of-a-deleted-module",
        ),
        pytest.param(
            ["solar-logger.ino.cpp", "alpha.pas"],
            "does not know how to configure a .pas file",
            id="unknown-source-kind",
        ),
        pytest.param(["alpha.cpp"], "No compile command for the copy", id="no-sketch-entry"),
    ],
)
def test_an_arduino_database_that_cannot_be_mapped_is_refused(project, relatives, message):
    sketch_dir, arduino_build, _, _ = project
    (sketch_dir / "alpha.pas").write_text("", encoding="utf-8")

    with pytest.raises(intellisense.GeneratorError, match=message):
        intellisense.project_units(
            [copy_entry(arduino_build, relative) for relative in relatives],
            arduino_build,
            sketch_dir,
        )


def test_check_reports_a_corrupt_database_instead_of_trusting_it(project, capsys):
    sketch_dir, arduino_build, _, _ = project
    output_dir = arduino_build.parent
    output_dir.mkdir(parents=True)
    (output_dir / intellisense.STATUS_NAME).write_text("{}", encoding="utf-8")
    (output_dir / intellisense.DATABASE_NAME).write_text("[{", encoding="utf-8")

    assert intellisense.check(sketch_dir, output_dir, arduino_build) == 1
    assert "not valid JSON" in capsys.readouterr().out


def test_the_fingerprint_moves_when_a_module_is_added_renamed_or_edited(project):
    sketch_dir = project[0]

    def current() -> str:
        return intellisense.fingerprint(
            sketch_dir, intellisense.fingerprinted_sources(sketch_dir)
        )

    seen = [current()]

    (sketch_dir / "delta.cpp").write_text("", encoding="utf-8")
    seen.append(current())

    (sketch_dir / "delta.cpp").rename(sketch_dir / "src" / "delta.cpp")
    seen.append(current())

    (sketch_dir / "alpha.h").write_text("// edited\n", encoding="utf-8")
    seen.append(current())

    assert len(set(seen)) == len(seen)
