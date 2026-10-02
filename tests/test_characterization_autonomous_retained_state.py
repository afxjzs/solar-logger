"""RTC-retained autonomous state: one definition each, and who may write it.

Written on 2026-10-02 before the ten `rtcAuto*` definitions moved out of the
sketch, and meant to fail until they had (D-043). Tests 1 and 2 are red against
the pre-move code. Test 3 is an absence guard that would also pass against an
empty module. Test 4 is green before the move: it is the compensating control
for the `extern`, not evidence that the move happened.

THE BOUNDARY
============

`autonomous_retained_state` owns exactly one definition of each retained
autonomous variable, in exactly one translation unit, with its retained
lifetime (`RTC_DATA_ATTR`) and its zero initializer, plus `AUTO_RTC_MAGIC`, the
value that says whether the rest is valid (D-011). It owns no transition: every
writer is scheduler, arm/stop, handoff or recovery policy, and stays where it
is. The header exports all ten by `extern`, which departs from D-047's "a
variable moves into a module only if that module writes it"; see D-063.

Deliberately not here:

  - `rtcSleepTestMagic` and `rtcSleepTestCycle`, which are power-test state
    under their own magic (D-011)
  - `AUTO_SEQ_BLOCK`, `reserveSequenceBlock()` and
    `ensureSequenceReservation()`: reservation policy owns the writes to the
    mirror `rtcAutoSeqHighWater`, and stays in the sketch until its own stage
"""

from __future__ import annotations

from pathlib import Path

import pytest

import firmware_source

from conftest import PROJECT_ROOT

MODULE_DIR = PROJECT_ROOT / "Arduino" / "solar-logger"
RETAINED_HEADER = MODULE_DIR / "autonomous_retained_state.h"
RETAINED_SOURCE = MODULE_DIR / "autonomous_retained_state.cpp"

# (type, name), in definition order.
RETAINED = (
    ("uint32_t", "rtcAutoMagic"),
    ("uint32_t", "rtcAutoBootId"),
    ("uint32_t", "rtcAutoSessionElapsedMs"),
    ("uint32_t", "rtcAutoIntervalStartMs"),
    ("uint32_t", "rtcAutoNextSeq"),
    ("uint32_t", "rtcAutoSeqHighWater"),
    ("int64_t", "rtcAutoRunChargeUAh"),
    ("int64_t", "rtcAutoRunEnergyUWh"),
    ("uint32_t", "rtcAutoCycleCount"),
    ("uint32_t", "rtcAutoCommandedSleepMs"),
)

# Every function allowed to write each one, measured on 2026-10-02 with
# compound assignment and increments included.
WRITERS = {
    "rtcAutoMagic": {
        "armAutonomousTest",
        "beginAutonomousSleepFromHostSession",
        "setup",
        "stopAutonomousTest",
    },
    "rtcAutoBootId": {
        "armAutonomousTest",
        "beginAutonomousSleepFromHostSession",
        "setup",
    },
    "rtcAutoSessionElapsedMs": {
        "armAutonomousTest",
        "autonomousDeepSleepAgain",
        "beginAutonomousSleepFromHostSession",
        "setup",
    },
    "rtcAutoIntervalStartMs": {
        "armAutonomousTest",
        "autonomousDeepSleepAgain",
        "beginAutonomousSleepFromHostSession",
        "runAutonomousWakeCycle",
        "setup",
    },
    "rtcAutoNextSeq": {
        "armAutonomousTest",
        "beginAutonomousSleepFromHostSession",
        "runAutonomousWakeCycle",
        "setup",
    },
    "rtcAutoSeqHighWater": {
        "autoStorageRecover",
        "reserveSequenceBlock",
    },
    "rtcAutoRunChargeUAh": {
        "autoStorageRecover",
        "clearStorage",
        "runAutonomousWakeCycle",
    },
    "rtcAutoRunEnergyUWh": {
        "autoStorageRecover",
        "clearStorage",
        "runAutonomousWakeCycle",
    },
    "rtcAutoCycleCount": {
        "armAutonomousTest",
        "beginAutonomousSleepFromHostSession",
        "runAutonomousWakeCycle",
        "setup",
    },
    "rtcAutoCommandedSleepMs": {
        "autonomousDeepSleepAgain",
    },
}

_ASSIGNMENTS = ("=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=")
_STEPS = ("++", "--")


def module_code(path: Path) -> firmware_source.Code:
    """One file, as tokens with comments stripped."""

    assert path.is_file(), f"{path.name} does not exist"
    return firmware_source.Code(
        firmware_source.tokenize(path.read_text(encoding="utf-8"), path.name)
    )


def write_positions(code: firmware_source.Code, name: str) -> list[int]:
    """Token indexes at which `name` is assigned, stepped or has its address
    taken. Taking the address is counted because it hands out write access."""

    texts = code.texts
    found = []

    for index, text in enumerate(texts):
        if text != name:
            continue

        after = texts[index + 1] if index + 1 < len(texts) else ""
        before = texts[index - 1] if index > 0 else ""

        if after in _ASSIGNMENTS or after in _STEPS or before in _STEPS or before == "&":
            found.append(index)

    return found


def test_each_retained_variable_is_defined_once_in_the_module():
    """1. The single highest-risk detail in BACKLOG's plan, made testable.

    Exactly one definition of each, with RTC_DATA_ATTR and a zero initializer,
    and it is in autonomous_retained_state.cpp. Anywhere else, a type followed
    by the name is a second definition.
    """

    source = module_code(RETAINED_SOURCE)

    for kind, name in RETAINED:
        assert source.count(f"RTC_DATA_ATTR {kind} {name} = 0 ;") == 1, (
            f"{RETAINED_SOURCE.name} must define {name} once, retained, zero"
        )

        for path in firmware_source.sketch_files():
            if path == RETAINED_SOURCE:
                continue

            code = module_code(path)
            definitions = [
                at
                for at in code.find_all(f"{kind} {name}")
                if at == 0 or code.texts[at - 1] != "extern"
            ]
            assert not definitions, f"{path.name} defines {name} a second time"


def test_the_header_declares_and_never_defines():
    """2. The header carries `extern` declarations only.

    A `static` variable in a header is the hazard BACKLOG names: it links
    cleanly and gives every translation unit that includes the header its own
    silent copy, so the autonomous session clock would reset without a word. A
    non-static definition in a header is the other mistake, and today it would
    link fine, because only one translation unit defines it; it would fail
    only once a second unit included the header. This test guards that future
    failure, not a present one.

    AUTO_RTC_MAGIC is the one value the header defines. A namespace-scope
    constexpr has internal linkage, so each unit gets its own copy of a
    constant, which is harmless and is not the `static` variable hazard.
    """

    header = module_code(RETAINED_HEADER)

    for kind, name in RETAINED:
        assert header.count(name) == 1, f"{name} appears more than once in the header"
        assert header.contains(f"extern {kind} {name} ;"), (
            f"{RETAINED_HEADER.name} must declare {name} extern, without an initializer"
        )

    for forbidden in ("RTC_DATA_ATTR", "static"):
        assert not header.contains(forbidden), (
            f"{RETAINED_HEADER.name} names `{forbidden}`; definitions belong in the .cpp"
        )

    assert header.count("=") == 1, "the header initializes something besides the magic"

    magic = "constexpr uint32_t AUTO_RTC_MAGIC = 0xA07011E5 ;"
    assert header.contains(magic)

    for path in firmware_source.sketch_files():
        if path != RETAINED_HEADER:
            assert not module_code(path).contains("AUTO_RTC_MAGIC ="), (
                f"{path.name} defines AUTO_RTC_MAGIC a second time"
            )


@pytest.mark.parametrize("path", (RETAINED_HEADER, RETAINED_SOURCE))
@pytest.mark.parametrize(
    "forbidden",
    (
        "rtcSleepTestMagic",           # power-test RTC state (D-011)
        "rtcSleepTestCycle",
        "SLEEP_POWER_TEST_RTC_MAGIC",
        "AUTO_SEQ_BLOCK",              # reservation policy owns the mirror's writes
        "reserveSequenceBlock",
        "ensureSequenceReservation",
        "loadSequenceHighWater",       # NVS
        "saveSequenceHighWater",
        "Preferences",
        "nextBootId",
        "LittleFS",                    # storage mechanics
        "autoStorageRecover",          # recovery composition
        "esp_deep_sleep_start",        # the scheduler
        "esp_sleep_enable_timer_wakeup",
        "Serial",
    ),
)
def test_the_module_takes_on_no_other_responsibility(path, forbidden):
    """3. Absence guard: would also pass against an empty module."""

    assert not module_code(path).contains(forbidden), (
        f"{path.name} names `{forbidden}`; that is not retained state's"
    )


def test_the_module_defines_no_function():
    """3. It owns no transition, and nothing in it runs before setup().

    No function, so no writer has moved in; no constructor, so the early
    timer-wake branch at the top of setup() cannot become a static
    initialization-order problem (D-020, D-024).
    """

    sketch = firmware_source.load()

    for name, definitions in sketch.functions.items():
        for definition in definitions:
            assert "autonomous_retained_state" not in definition.where, (
                f"{name}() is defined in the retained-state module"
            )


def test_only_the_known_functions_write_retained_state():
    """4. The compensating control for the `extern`.

    Anything that includes the header can now write these ten values, which is
    the header's one real cost. This pins every writer of every one in one
    place, so a new writer is a failing test rather than an unmonitored change.
    Green before the move as well as after: it is not evidence that the move
    happened.

    Limit: a write through a reference parameter is invisible to a token search.
    None exists today; taking an address with `&` is counted as a write.
    """

    sketch = firmware_source.load()

    for _, name in RETAINED:
        writers = {
            function
            for function, definitions in sketch.functions.items()
            for definition in definitions
            if write_positions(definition.body, name)
        }
        assert writers == WRITERS[name], f"{name} writers changed: {sorted(writers)}"

        in_bodies = sum(
            len(write_positions(definition.body, name))
            for definitions in sketch.functions.values()
            for definition in definitions
        )
        assert len(write_positions(sketch.code, name)) == in_bodies + 1, (
            f"{name} is written outside a function body other than at its definition"
        )
