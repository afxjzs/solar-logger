"""Sequence authority: the D-023 decision, and the boundary around it.

Written on 2026-10-01 before the decision moved out of the sketch, and meant to
fail until it had (D-043).

EXECUTED ELSEWHERE
==================

What the decision decides is already executed on the host, through
`autoStorageRecover()`, by tests/test_characterization_storage_recovery.py:
an intact log is the authority, and a partial, corrupt, unreadable or empty one
falls back to one past the NVS reservation. Those tests are the behavioral
contract for this move and do not change. This file pins only the boundary.

THE BOUNDARY
============

`sequence_authority` holds the reconciliation of what the durable log says
against what the NVS reservation floor says, and nothing else. It is handed the
scan and the reservation; it reads no NVS, no RTC-retained variable and no
filesystem itself, so every dependency it has is a parameter (D-050) and it
declares no `extern` (D-058).

Deliberately left in the sketch:

  - reading the reservation out of NVS into the RTC mirror
    `rtcAutoSeqHighWater`, which is RTC-retained autonomous state
  - `reserveSequenceBlock()`, `ensureSequenceReservation()` and
    `AUTO_SEQ_BLOCK`, which write that mirror
  - reseeding the running totals (D-060, D-061), which is retained state too

DOCSTRING CORRECTED 2026-10-02, no assertion changed. This said the mirror
"itself" was left in the sketch. D-063 moved its definition into
`autonomous_retained_state.cpp`; every write to it stayed in the sketch, which
is what the three bullets above are about. The assertions below are unaffected
and were not touched: they forbid `rtcAutoSeqHighWater`, `AUTO_SEQ_BLOCK`,
`saveSequenceHighWater` and `extern` in `sequence_authority`'s own two files,
and all four are still absent from them.
"""

from __future__ import annotations

from pathlib import Path

import pytest

import firmware_source

from conftest import PROJECT_ROOT

MODULE_DIR = PROJECT_ROOT / "Arduino" / "solar-logger"
SEQUENCE_HEADER = MODULE_DIR / "sequence_authority.h"
SEQUENCE_SOURCE = MODULE_DIR / "sequence_authority.cpp"

DECISION = "sequenceAuthorityNext"


def module_code(path: Path) -> firmware_source.Code:
    """One file, as tokens with comments stripped."""

    return firmware_source.Code(
        firmware_source.tokenize(path.read_text(encoding="utf-8"), path.name)
    )


def test_the_d023_decision_is_made_in_exactly_one_place():
    """One definition, in the module, and the sketch no longer reconciles."""

    sketch = firmware_source.load()

    definitions = sketch.functions.get(DECISION, [])
    assert len(definitions) == 1, f"{DECISION}() is defined {len(definitions)} times"
    assert definitions[0].where.startswith("Arduino/solar-logger/sequence_authority.cpp")

    assert module_code(SEQUENCE_HEADER).contains(
        f"uint32_t {DECISION} ( const AutoLogScan & scan , uint32_t reservedHighWater ) ;"
    )

    for path in firmware_source.sketch_files():
        if path != SEQUENCE_SOURCE:
            assert not module_code(path).contains("logIntact"), (
                f"{path.name} decides whether the log tail is the authority; "
                "sequence_authority.cpp owns that"
            )


def test_recovery_hands_the_decision_the_floor_it_just_read():
    """The order the serial transcript depends on, pinned at the one caller.

    The reservation is read into the mirror, then the decision prints its
    lines, then the totals are reseeded, exactly as when this was one function.
    """

    sketch = firmware_source.load()

    assert sketch.callers(DECISION) == {"autoStorageRecover"}

    recover = sketch.function("autoStorageRecover")
    load = recover.index("rtcAutoSeqHighWater = loadSequenceHighWater ( ) ;")
    decide = recover.index(
        f"uint32_t next = {DECISION} ( scan , rtcAutoSeqHighWater ) ;"
    )
    reseed = recover.index("rtcAutoRunChargeUAh = scan . lastRunChargeUAh ;")

    assert load < decide < reseed


@pytest.mark.parametrize("path", (SEQUENCE_HEADER, SEQUENCE_SOURCE))
@pytest.mark.parametrize(
    "forbidden",
    (
        "extern",                    # D-058: state goes behind a parameter
        "RTC_DATA_ATTR",             # retained autonomous state
        "rtcAutoSeqHighWater",       # the mirror the caller maintains
        "rtcAutoNextSeq",
        "rtcAutoRunChargeUAh",       # the totals reseed (D-060, D-061)
        "rtcAutoMagic",
        "retainedStateValid",
        "Preferences",               # NVS
        "loadSequenceHighWater",     # the caller reads the floor
        "saveSequenceHighWater",     # reservation stays with the mirror
        "AUTO_SEQ_BLOCK",
        "LittleFS",                  # storage mechanics
        "autoStorageScanAndRepair",  # the scan is handed in, not taken
        "esp_deep_sleep_start",      # the scheduler
    ),
)
def test_the_sequence_module_takes_on_no_other_responsibility(path, forbidden):
    """Every name here belongs to a boundary D-055 keeps separate."""

    assert not module_code(path).contains(forbidden), (
        f"{path.name} names `{forbidden}`; that is not sequence authority's"
    )
