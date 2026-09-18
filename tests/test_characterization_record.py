"""Characterization of the on-flash record contract.

Part of the pre-modularization gate (docs/DECISIONS.md D-043). Experiment 3's
log is thousands of 72-byte records written by this layout, and nothing may
change how the next image reads them.

The compile-time `static_assert(sizeof(AutoRecord) == 72)` stays in the
firmware and is asserted present by test_autonomous_accounting.py. It catches a
size change. It does NOT catch two same-width fields swapping places, a signed
field becoming unsigned, or a field being assigned after the CRC was computed.
Those are exactly the mistakes a file move invites, so they are pinned here.

What this cannot do is run the CRC. `esp_rom_crc32_le` is ESP32 ROM code, and
no captured dump of real record bytes exists to test a host implementation
against, so only which bytes are covered is checked, not the checksum values.
"""

from __future__ import annotations

import firmware_source

# STORAGE_SYNC_DESIGN.md Section 5, as the firmware implements it.
# (type, field, byte offset)
FROZEN_LAYOUT = (
    ("uint16_t", "magic", 0),
    ("uint8_t", "version", 2),
    ("uint8_t", "flags", 3),
    ("uint32_t", "seq", 4),
    ("int64_t", "running_charge_uAh", 8),
    ("int64_t", "running_energy_uWh", 16),
    ("uint32_t", "experiment_id", 24),
    ("uint32_t", "boot_id", 28),
    ("uint32_t", "session_elapsed_ms", 32),
    ("uint32_t", "epoch_s", 36),
    ("uint32_t", "interval_ms", 40),
    ("uint32_t", "bus_uV", 44),
    ("int32_t", "avg_current_uA", 48),
    ("int32_t", "avg_power_uW", 52),
    ("int32_t", "temp_mC", 56),
    ("int32_t", "interval_charge_uAh", 60),
    ("int32_t", "interval_energy_uWh", 64),
    ("uint32_t", "crc32", 68),
)

WIDTH = {"uint8_t": 1, "uint16_t": 2, "uint32_t": 4, "int32_t": 4, "int64_t": 8}

# Values that give stored bytes their meaning. Changing any of them changes how
# every existing record reads, so each is a record version decision.
FROZEN_CONSTANTS = (
    "constexpr uint16_t AUTO_RECORD_MAGIC = 0xB5A5;",
    "constexpr uint8_t AUTO_RECORD_VERSION = 1;",
    "constexpr size_t AUTO_RECORD_SIZE = sizeof(AutoRecord);",
    "constexpr uint8_t AUTO_FLAG_TIME_QUALITY_MASK = 0x03;",
    "constexpr uint8_t AUTO_TIME_UNKNOWN = 0;",
    "constexpr uint8_t AUTO_TIME_SYNCHRONIZED = 1;",
    "constexpr uint8_t AUTO_TIME_HOLDOVER = 2;",
    "constexpr uint8_t AUTO_FLAG_FIRST_AFTER_BOOT = 0x04;",
    "constexpr uint8_t AUTO_FLAG_ACCUM_SUSPECT = 0x08;",
    "constexpr uint8_t AUTO_FLAG_INTERVAL_ODD = 0x10;",
    "constexpr uint8_t AUTO_FLAG_INA_MATHOF = 0x20;",
    "constexpr uint8_t AUTO_FLAG_INA_ACCUM_OF = 0x40;",
    "constexpr uint8_t AUTO_FLAG_EXPERIMENT_UNKNOWN = 0x80;",
    "constexpr uint32_t AUTO_EXPERIMENT_UNKNOWN = UINT32_MAX;",
)


def test_auto_record_layout_is_frozen():
    """Field order, width, signedness and packing - the bytes on flash."""

    sketch = firmware_source.load()
    members = sketch.struct("AutoRecord").texts

    declared = [
        (members[index], members[index + 1])
        for index in range(0, len(members), 3)
    ]
    assert members[2::3] == [";"] * len(declared), (
        "AutoRecord must be plain `type name;` members: no arrays, bitfields or "
        "initializers"
    )

    assert declared == [(kind, name) for kind, name, _ in FROZEN_LAYOUT]

    offset = 0
    for kind, name, expected in FROZEN_LAYOUT:
        assert offset == expected, f"{name} would sit at byte {offset}, not {expected}"
        offset += WIDTH[kind]

    assert offset == 72

    # Packed, so the layout above is the layout on every compiler.
    assert sketch.struct_head("AutoRecord").contains("__attribute__((packed))")


def test_auto_record_crc_seal_and_validation_are_frozen():
    """CRC covers bytes 0-67; it is computed last; validity is magic+version+CRC."""

    sketch = firmware_source.load()

    for constant in FROZEN_CONSTANTS:
        assert sketch.code.contains(constant), f"changed or missing: {constant}"

    # CRC-32 over every byte before the trailing crc32 field - the first 68,
    # given the layout above - seeded with 0.
    assert sketch.function("autoRecordCrc").texts == firmware_source.words(
        "return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t *>(&record), "
        "AUTO_RECORD_SIZE - sizeof(uint32_t));"
    )

    assert sketch.function("autoRecordValid").texts == firmware_source.words(
        "if (record.magic != AUTO_RECORD_MAGIC) { return false; }"
        "if (record.version != AUTO_RECORD_VERSION) { return false; }"
        "return record.crc32 == autoRecordCrc(record);"
    )

    # The one place a record is built. The CRC must be computed after the last
    # field is written and before the append, or the log fills with records
    # that fail their own validation.
    wake = sketch.function("runAutonomousWakeCycle")
    seal = wake.index("record.crc32 = autoRecordCrc(record);")
    field_writes = [
        index
        for index in range(len(wake) - 2)
        if wake.texts[index : index + 2] == ["record", "."]
        and wake.texts[index + 3] == "="
    ]

    assert field_writes[-1] == seal, "a record field is written after the CRC"
    assert seal < wake.index("autoStorageAppend(record)")

    # The record is written and read back as exactly AUTO_RECORD_SIZE raw bytes.
    assert sketch.function("autoStorageAppend").contains(
        "file.write(reinterpret_cast<const uint8_t *>(&record), AUTO_RECORD_SIZE)"
    )
    assert sketch.callers("autoStorageAppend") == {"runAutonomousWakeCycle"}
