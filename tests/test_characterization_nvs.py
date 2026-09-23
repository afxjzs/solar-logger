"""Characterization of what is stored in NVS, and of the persistence boundary.

Part of the pre-modularization gate (docs/DECISIONS.md D-043), written with the
NVS extraction that created `nvs_persistence.h` / `.cpp`.

Experiment 3 is live on the board. Its experiment id, interval number and
running totals are five key/value pairs in the `solarlog` namespace, and the
autonomous log's sequence floor is a sixth. A renamed key, a changed width, or
a default quietly substituted for a failed read does not fail to compile and
does not error at runtime: the board simply comes up as a different experiment,
or reuses sequence numbers a host has already stored. So the stored layout is
pinned here by name and by type.

Read as tokens through tests/firmware_source.py, which reads every file Arduino
compiles, so nothing here breaks when code moves between files again.

TWO TESTS ARE DELIBERATELY FILE-SPECIFIC, and they are the boundary itself
rather than the shape of any function: exactly one file may open the namespace,
and it may not be the sketch. Everything else is location-independent.

NOT DUPLICATED HERE: that a failed experiment-id read cannot become experiment
0. tests/test_autonomous_accounting.py already pins it (D-024).
"""

from __future__ import annotations

import pytest

import firmware_source

# The namespace, and every key in it, as the shipped board holds them.
NVS_NAMESPACE = "solarlog"

# The experiment checkpoint: key -> (writer, reader, the width the writer
# checks). Schema 2 is what Experiment 3 is stored under.
CHECKPOINT_KEYS = {
    "schema": ("putUInt", "getUInt", "uint32_t"),
    "experiment": ("putUInt", "getUInt", "uint32_t"),
    "interval": ("putUInt", "getUInt", "uint32_t"),
    "charge": ("putDouble", "getDouble", "double"),
    "energy": ("putDouble", "getDouble", "double"),
}

# Everything else in the namespace: constant name -> stored key.
NAMED_KEYS = {
    "NVS_WIFI_POWER_TEST_KEY": "wifi_test_armed",
    "NVS_SLEEP_POWER_TEST_KEY": "sleep_test_arm",
    "NVS_SLEEP_INA_OFF_KEY": "sleep_ina_off",
    "NVS_AUTO_TEST_KEY": "auto_test_arm",
    "NVS_AUTO_INTERVAL_KEY": "auto_int_s",
    "NVS_AUTO_SEQ_HW_KEY": "auto_seq_hw",
    "NVS_BOOT_ID_KEY": "boot_id",
}

# (loader, saver, key constant) for the three persisted power-test flags.
POWER_TEST_FLAGS = (
    pytest.param(
        "loadWifiPowerTestArmed",
        "saveWifiPowerTestArmed",
        "NVS_WIFI_POWER_TEST_KEY",
        id="wifi_armed",
    ),
    pytest.param(
        "loadSleepPowerTestArmed",
        "saveSleepPowerTestArmed",
        "NVS_SLEEP_POWER_TEST_KEY",
        id="sleep_armed",
    ),
    pytest.param(
        "loadSleepPowerTestInaOff",
        "saveSleepPowerTestInaOff",
        "NVS_SLEEP_INA_OFF_KEY",
        id="sleep_ina_off",
    ),
)


@pytest.fixture(scope="module")
def sketch():
    return firmware_source.load()


def first_arguments(code: firmware_source.Code, call: str) -> list[str]:
    """First argument token of every call matching `call`, which ends in `(`.

    Code.call_arguments() takes a single-token name; these calls are through a
    member, `preferences.putUInt(`, which is four tokens.
    """

    width = len(firmware_source.words(call))
    return [code.texts[start + width] for start in code.find_all(call)]


def file_code(name: str) -> firmware_source.Code:
    """One sketch file on its own, as tokens."""

    path = firmware_source.SKETCH_DIR / name
    return firmware_source.Code(
        firmware_source.tokenize(path.read_text(encoding="utf-8"), name)
    )


# ============================================================================
# The namespace and the keys
# ============================================================================


def test_the_namespace_is_still_solarlog(sketch):
    """A different namespace is a board with no experiment and no complaint."""

    definitions = sketch.code.find_all(f'NVS_NAMESPACE[] = "{NVS_NAMESPACE}"')

    assert len(definitions) == 1, (
        f"expected exactly one definition of the {NVS_NAMESPACE!r} namespace"
    )


@pytest.mark.parametrize("constant,key", sorted(NAMED_KEYS.items()))
def test_each_named_key_keeps_its_stored_spelling(sketch, constant, key):
    """The key a constant resolves to is what is actually on the flash."""

    assert sketch.code.contains(f'{constant}[] = "{key}"'), (
        f"{constant} must still name the stored key {key!r}"
    )


@pytest.mark.parametrize("key", sorted(set(NAMED_KEYS.values()) | set(CHECKPOINT_KEYS)))
def test_every_key_fits_the_esp32_fifteen_character_limit(key):
    """ESP32 NVS rejects a longer name, which is why it is `sleep_test_arm`.

    D-011. A rejected key is a value that is never stored, and the writer sees
    a short write rather than an explanation.
    """

    assert len(key) <= 15, f"{key!r} is {len(key)} characters; NVS allows 15"


# ============================================================================
# The experiment checkpoint
# ============================================================================


def test_the_checkpoint_struct_keeps_its_field_types(sketch):
    """The struct is the new path between the experiment globals and NVS.

    A field narrowed here loses precision before anything reaches the flash,
    and the stored value would still be the right width.
    """

    members = sketch.struct("LoggerCheckpoint")

    for declaration in (
        "uint32_t experimentId;",
        "uint32_t completedInterval;",
        "double runningCharge_mAh;",
        "double runningEnergy_mWh;",
    ):
        assert members.contains(declaration), f"LoggerCheckpoint lost `{declaration}`"


def test_the_checkpoint_writes_exactly_the_five_known_keys(sketch):
    """No key added, none dropped, none renamed."""

    save = sketch.function("saveCheckpoint")
    written = {
        firmware_source.unquote(argument)
        for name in ("putUInt", "putDouble")
        for argument in first_arguments(save, f"preferences.{name}(")
    }

    assert written == set(CHECKPOINT_KEYS)


@pytest.mark.parametrize("key", sorted(CHECKPOINT_KEYS))
def test_each_checkpoint_key_keeps_its_width(sketch, key):
    """Writer, reader and the checked width all still agree for this key."""

    writer, reader, width = CHECKPOINT_KEYS[key]

    save = sketch.function("saveCheckpoint")
    load = sketch.function("loadCheckpoint")

    assert save.contains(f'preferences.{writer}("{key}",'), (
        f"{key} must still be written with {writer}"
    )
    assert load.contains(f'preferences.{reader}("{key}",'), (
        f"{key} must still be read with {reader}"
    )
    assert save.contains(f"!= sizeof({width})"), (
        f"the short-write check for {key} must still compare against {width}"
    )


def test_a_failed_checkpoint_write_is_reported_for_every_key(sketch):
    """Every key is attempted, and any failure reaches the return value.

    Stopping at the first failure would leave the console naming one key when
    several did not write, and returning true after any of them would let
    `CMD_RESULT,...,OK` follow a checkpoint that is not on the flash.
    """

    save = sketch.function("saveCheckpoint").without_serial_output()

    assert save.block_after("if (!preferences.begin(NVS_NAMESPACE, false))").contains(
        "return false;"
    ), "a namespace that will not open must fail, not fall through"

    assert save.count("ok = false;") == len(CHECKPOINT_KEYS), (
        "each key's short-write branch must record the failure"
    )
    assert not save.contains("ok = false; return"), (
        "a failed key must not abandon the remaining keys"
    )
    assert save.texts[-3:] == firmware_source.words("return ok;"), (
        "saveCheckpoint must end by returning what actually happened"
    )


def test_an_uninterpretable_checkpoint_read_fails_rather_than_zeroing(sketch):
    """An incomplete or unknown-schema store is not a fresh experiment.

    Returning true with zeros here would silently restart Experiment 3 at
    interval 0 with no running totals, and the caller would checkpoint that
    over the real state one interval later.
    """

    load = sketch.function("loadCheckpoint").without_serial_output()

    for guard in (
        'if (!preferences.isKey("interval") || !preferences.isKey("charge") '
        '|| !preferences.isKey("energy"))',
        'if (!preferences.isKey("experiment") || !preferences.isKey("interval") '
        '|| !preferences.isKey("charge") || !preferences.isKey("energy"))',
    ):
        assert load.block_after(guard).contains("return false;"), (
            f"an incomplete checkpoint must fail: {guard}"
        )

    assert load.texts[-3:] == firmware_source.words("return false;"), (
        "an unsupported schema must fail rather than fall through to success"
    )


def test_both_checkpoint_writers_check_the_result(sketch):
    """Nothing may carry on as though a checkpoint had been stored."""

    callers = sketch.callers("saveCheckpoint")
    assert callers == {"resetExperiment", "closeMeasurementInterval"}

    for name in callers:
        assert sketch.function(name).contains("if (!saveCheckpoint("), (
            f"{name}() must branch on the checkpoint result"
        )


# ============================================================================
# Power-test flags
# ============================================================================


@pytest.mark.parametrize("loader,saver,constant", POWER_TEST_FLAGS)
def test_a_power_test_flag_keeps_its_key_and_its_default(
    sketch, loader, saver, constant
):
    """The flag is read from and written to the same key, and defaults to off.

    D-006, D-011 and D-014 keep these three out of the experiment checkpoint on
    purpose. A flag that defaulted to armed, or read a key the writer does not
    write, would run a bench test nobody asked for on battery power.
    """

    load = sketch.function(loader)
    save = sketch.function(saver)

    assert save.contains(f"preferences.putBool({constant},")
    assert load.contains(f"preferences.getBool({constant}, false)"), (
        "an absent value must read as not armed"
    )
    assert load.contains(f"if (!preferences.isKey({constant}))")


@pytest.mark.parametrize("loader,saver,constant", POWER_TEST_FLAGS)
def test_a_power_test_flag_that_cannot_be_read_is_reported_not_armed(
    sketch, loader, saver, constant
):
    """The loader assigns the value it just promised on the console.

    Leaving the caller's variable alone would make the printed claim depend on
    whatever it already held (D-016: never present something plausible in place
    of something real).
    """

    load = sketch.function(loader)
    failure = load.block_after("if (!preferences.begin(NVS_NAMESPACE, true))")

    assert failure.contains("= false;"), (
        "a flag that cannot be read must be set to not armed, not left alone"
    )
    assert failure.contains("return true;")


def test_a_failed_power_test_flag_write_is_reported(sketch):
    """Arming is refused when the flag did not reach the flash."""

    for saver in ("saveWifiPowerTestArmed", "saveSleepPowerTestArmed",
                  "saveSleepPowerTestInaOff"):
        save = sketch.function(saver).without_serial_output()

        assert save.block_after(
            "if (!preferences.begin(NVS_NAMESPACE, false))"
        ).contains("return false;")
        assert save.block_after("if (written != sizeof(bool))").contains(
            "return false;"
        )


# ============================================================================
# Sequence reservation
# ============================================================================


def test_the_sequence_reservation_keeps_its_key_and_width(sketch):
    """The NVS floor that makes a duplicate sequence impossible (D-017, D-023)."""

    save = sketch.function("saveSequenceHighWater")
    load = sketch.function("loadSequenceHighWater")

    assert save.contains("preferences.putUInt(NVS_AUTO_SEQ_HW_KEY, highWater)")
    assert save.contains("if (written != sizeof(uint32_t))")
    assert load.contains("preferences.getUInt(NVS_AUTO_SEQ_HW_KEY, 0)")


def test_the_reservation_floor_rises_only_after_the_write_succeeds(sketch):
    """A reservation that was not stored must not raise the RTC floor.

    D-023: a gap is acceptable, a duplicate never is. If the RTC mirror moved
    on a failed write, the next boot would believe a block was reserved that
    NVS has no record of, and could hand out a sequence number already used.
    """

    reserve = sketch.function("reserveSequenceBlock").without_serial_output()

    assert reserve.contains("uint32_t newHighWater = fromSeq + AUTO_SEQ_BLOCK;")
    assert reserve.block_after("if (!saveSequenceHighWater(newHighWater))").contains(
        "return false;"
    )
    assert reserve.index("saveSequenceHighWater (") < reserve.index(
        "rtcAutoSeqHighWater = newHighWater;"
    ), "the RTC floor must move only after the NVS write reported success"


def test_the_default_cadence_still_passes_the_range_check(sketch):
    """The one assumption the extraction made, guarded at compile time.

    The read moved into the module, so its early return on a failed open now
    lands in loadAutonomousSettings() BEFORE the range check rather than after
    it. That is only behavior-preserving while the default is itself in range.
    """

    assert sketch.code.contains(
        "static_assert(AUTO_INTERVAL_SECONDS_DEFAULT >= AUTO_INTERVAL_SECONDS_MIN "
        "&& AUTO_INTERVAL_SECONDS_DEFAULT <= AUTO_INTERVAL_SECONDS_MAX,"
    )


def test_the_block_width_is_still_sixty_four(sketch):
    """D-023 states the block size; nothing else in the log records it."""

    assert sketch.code.contains("AUTO_SEQ_BLOCK = 64;")


# ============================================================================
# The module boundary
# ============================================================================


def test_only_one_file_opens_the_nvs_namespace():
    """One owner for the Preferences object, the namespace and every key.

    File-specific on purpose: this is the boundary the extraction created, not
    the shape of a function. Two files reaching for `preferences` is how the
    stored layout starts drifting apart in two places.
    """

    owners = [
        path.name
        for path in firmware_source.sketch_files()
        if firmware_source.Code(
            firmware_source.tokenize(path.read_text(encoding="utf-8"), path.name)
        ).contains("preferences.")
    ]

    assert len(owners) == 1, f"exactly one file may use Preferences, found: {owners}"


def test_the_sketch_no_longer_reaches_into_nvs_itself():
    """What the extraction achieved, stated as a test.

    The sketch decides WHEN a value changes and calls down; it must not name a
    key, open the namespace, or hold the Preferences object.
    """

    ino = file_code("solar-logger.ino")

    assert not ino.contains("preferences."), "the sketch must not open NVS directly"
    assert not ino.contains("Preferences"), "the Preferences object is not the sketch's"
    assert not ino.contains("NVS_NAMESPACE")

    for constant in NAMED_KEYS:
        assert not ino.contains(constant), f"{constant} must stay inside the module"

    for key in sorted(set(NAMED_KEYS.values()) | set(CHECKPOINT_KEYS)):
        assert not ino.contains(f'"{key}"'), f"the sketch must not name the key {key!r}"
