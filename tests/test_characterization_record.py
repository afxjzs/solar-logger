"""Characterization of the on-flash record contract.

Part of the pre-modularization gate (docs/DECISIONS.md D-043). Experiment 3's
log is thousands of 72-byte records written by this layout, and nothing may
change how the next image reads them.

The compile-time `static_assert(sizeof(AutoRecord) == 72)` stays in the
firmware and is asserted present by test_autonomous_accounting.py. It catches a
size change. It does NOT catch two same-width fields swapping places, a signed
field becoming unsigned, or a field being assigned after the CRC was computed.
Those are exactly the mistakes a file move invites, so they are pinned here.

Since 2026-09-23 this file also pins the layout against a REAL deployed record.
A `LOGGER STORAGE DUMP` line from Experiment 3 was captured, and rebuilding its
72 bytes from the layout below reproduces the CRC the board itself stored. That
single match settles what source reading could not: the exact field offsets,
widths, signedness and little-endian packing, the covered byte range, and the
`esp_rom_crc32_le(0, ...)` convention. An earlier version of this docstring said
running the CRC on the host was impossible for want of real bytes; it is not.
"""

from __future__ import annotations

import struct
import zlib

import pytest

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

# struct.pack codes for the same types, little-endian, no padding. Used only to
# rebuild the golden record below; WIDTH above stays the authority on size.
PACK = {
    "uint8_t": "B",
    "uint16_t": "H",
    "uint32_t": "I",
    "int32_t": "i",
    "int64_t": "q",
}

# ============================================================================
# GOLDEN RECORD - REAL DEPLOYED EXPERIMENT 3 DATA, CAPTURED 2026-09-23
# ============================================================================
#
# Verbatim source line, from LOGGER STORAGE DUMP on the board running
# Experiment 3. Do not edit it to make a test pass: it is evidence, and the
# firmware produced it.
#
#   [STORAGE] #3033 seq=4445 boot=26 exp=3 elapsed_ms=58629900
#   interval_ms=60004 V=13.051367 I_mA=-0.616 P_mW=8.025 T_C=18.695
#   dQ_uAh=-10 dE_uWh=129 Qsum_uAh=380771 Esum_uWh=5602351 time=UNKNOWN
#   epoch=0 flags=0x0[none] crc=0xFB25DA73
#
# The dump prints every field except `magic` and `version`, which are fixed for
# any record that validated, and prints the snapshot fields scaled but at exact
# microunit resolution (V to 6 decimals is microvolts; I_mA and P_mW to 3 are
# microamps and microwatts; T_C to 3 is millidegrees), so all 72 bytes are
# recoverable. The stored CRC is what checks the recovery.
GOLDEN_FIELDS = {
    "magic": 0xB5A5,
    "version": 1,
    "flags": 0x00,
    "seq": 4445,
    "running_charge_uAh": 380771,
    "running_energy_uWh": 5602351,
    "experiment_id": 3,
    "boot_id": 26,
    "session_elapsed_ms": 58629900,
    "epoch_s": 0,
    "interval_ms": 60004,
    "bus_uV": 13051367,
    "avg_current_uA": -616,
    "avg_power_uW": 8025,
    "temp_mC": 18695,
    "interval_charge_uAh": -10,
    "interval_energy_uWh": 129,
}

GOLDEN_CRC = 0xFB25DA73
GOLDEN_COVERED_BYTES = 68


def golden_covered_bytes() -> bytes:
    """The 68 CRC-covered bytes, packed from FROZEN_LAYOUT."""

    blob = b""
    for kind, name, offset in FROZEN_LAYOUT:
        if name == "crc32":
            continue
        assert len(blob) == offset, f"{name} packed to {len(blob)}, not {offset}"
        blob += struct.pack("<" + PACK[kind], GOLDEN_FIELDS[name])

    return blob


def golden_record_bytes() -> bytes:
    """All 72 bytes, covered range plus the CRC the board stored."""

    return golden_covered_bytes() + struct.pack("<I", GOLDEN_CRC)


def auto_record_crc(covered: bytes) -> int:
    """Host equivalent of autoRecordCrc().

    `esp_rom_crc32_le(0, buf, len)` is standard IEEE 802.3 CRC-32: reflected
    polynomial 0xEDB88320, initial value 0xFFFFFFFF, final XOR 0xFFFFFFFF. That
    is exactly `zlib.crc32`, and the golden record below is what proves it
    rather than the naming. The ROM function's `0` seed is pre-inverted
    internally, which is why seeding zlib with nothing is the correct analogue.
    """

    return zlib.crc32(covered) & 0xFFFFFFFF

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


# ============================================================================
# BINARY COMPATIBILITY AGAINST REAL DEPLOYED DATA
# ============================================================================


def test_golden_record_is_exactly_72_bytes():
    assert len(golden_covered_bytes()) == GOLDEN_COVERED_BYTES
    assert len(golden_record_bytes()) == 72


def test_golden_record_reproduces_the_crc_the_board_stored():
    """The whole layout, proved by one number.

    This is the strongest statement in this file. The CRC is computed over all
    68 preceding bytes, so it can only match if every field's offset, width,
    signedness and byte order is identical to the firmware that wrote it, the
    struct really is packed with no padding, and the covered range really is
    bytes 0..67. Any single one of those being wrong changes the checksum.
    """

    assert auto_record_crc(golden_covered_bytes()) == GOLDEN_CRC


def test_golden_record_crc_is_stored_little_endian_at_offset_68():
    record = golden_record_bytes()

    assert record[68:72] == bytes((0x73, 0xDA, 0x25, 0xFB))
    assert struct.unpack_from("<I", record, 68)[0] == GOLDEN_CRC


def test_golden_record_magic_and_version_are_the_frozen_constants():
    record = golden_record_bytes()

    assert struct.unpack_from("<H", record, 0)[0] == 0xB5A5
    assert record[2] == 1
    assert record[3] == 0x00, "flags=0x0[none] in the captured dump"


@pytest.mark.parametrize("offset", range(GOLDEN_COVERED_BYTES))
def test_mutating_any_covered_byte_breaks_the_crc(offset):
    """All 68 of them. The covered range is not asserted, it is demonstrated."""

    corrupted = bytearray(golden_covered_bytes())
    corrupted[offset] ^= 0xFF

    assert auto_record_crc(bytes(corrupted)) != GOLDEN_CRC


@pytest.mark.parametrize("bit", (0, 7, 15, 23, 31))
def test_mutating_the_stored_crc_breaks_validation(bit):
    """A flipped CRC must fail even though every covered byte is untouched."""

    record = bytearray(golden_record_bytes())
    record[68 + bit // 8] ^= 1 << (bit % 8)

    stored = struct.unpack_from("<I", record, 68)[0]

    assert stored != GOLDEN_CRC
    assert auto_record_crc(bytes(record[:GOLDEN_COVERED_BYTES])) != stored


def test_the_crc_does_not_cover_the_crc_field():
    """Seeding over all 72 bytes must NOT reproduce the stored value.

    Guards the one-off that looks harmless: covering the trailing field too.
    """

    assert auto_record_crc(golden_record_bytes()) != GOLDEN_CRC


def test_golden_record_snapshot_fields_match_the_printed_dump():
    """The reconstruction is traceable back to the transcript, not fitted to it."""

    assert GOLDEN_FIELDS["bus_uV"] / 1_000_000 == pytest.approx(13.051367)
    assert GOLDEN_FIELDS["avg_current_uA"] / 1_000 == pytest.approx(-0.616)
    assert GOLDEN_FIELDS["avg_power_uW"] / 1_000 == pytest.approx(8.025)
    assert GOLDEN_FIELDS["temp_mC"] / 1_000 == pytest.approx(18.695)


def test_golden_record_covers_every_field_in_the_frozen_layout():
    """A field added to the layout without a golden value fails here."""

    named = {name for _, name, _ in FROZEN_LAYOUT if name != "crc32"}

    assert set(GOLDEN_FIELDS) == named


def test_signed_fields_in_the_golden_record_really_are_signed():
    """`avg_current_uA` and `interval_charge_uAh` are negative in real data.

    Recorded because it is the accident this fixture would otherwise invite: a
    signed field flipped to unsigned still packs identically for positive values,
    so a golden record full of positive numbers would not catch it. These two are
    negative on the deployed board, so the CRC test above does catch it.

    Charge is negative while energy is positive because the INA228 reports them
    differently: SLYS021A Table 7-15 makes CHARGE a "Two's complement value",
    while Table 7-14 makes ENERGY "Unsigned representation. Positive value."
    See docs/BACKLOG.md - the asymmetry is a documented open issue, not a bug
    this fixture should paper over.
    """

    assert GOLDEN_FIELDS["avg_current_uA"] < 0
    assert GOLDEN_FIELDS["interval_charge_uAh"] < 0
    assert GOLDEN_FIELDS["avg_power_uW"] > 0
    assert GOLDEN_FIELDS["interval_energy_uWh"] > 0


# ============================================================================
# DIAG_ALRT READ ORDER
# ============================================================================


def test_diag_alrt_is_read_before_the_accumulators():
    """The ordering IS the correctness, so it is pinned rather than commented.

    INA228 datasheet SLYS021A Table 7-16: ENERGYOF "Clears when the ENERGY
    register is read", CHARGEOF "Clears when the CHARGE register is read".
    Reading an accumulator destroys its own overflow flag, so DIAG_ALRT has to be
    sampled BEFORE both reads. Being in the same wake is not sufficient.

    Until 2026-09-23 this firmware read the accumulators first, which made
    AUTO_FLAG_INA_ACCUM_OF unreachable for a real overflow while every record
    still reported "no accumulator overflow". Nothing errored, so only the order
    itself can catch a regression - including one caused by a later file move.
    """

    wake = firmware_source.load().function("runAutonomousWakeCycle")

    diag = wake.index("readRegister16(REG_DIAG_ALRT, diag)")
    charge = wake.index("readAccumulatedCharge_mAh(intervalCharge_mAh)")
    energy = wake.index("readAccumulatedEnergy_mWh(intervalEnergy_mWh)")

    assert diag < charge, "DIAG_ALRT is read after CHARGE; CHARGEOF is already gone"
    assert diag < energy, "DIAG_ALRT is read after ENERGY; ENERGYOF is already gone"


def test_the_overflow_flags_are_still_acted_on_after_the_reorder():
    """The reorder must not have changed which flags an overflow sets."""

    wake = firmware_source.load().function("runAutonomousWakeCycle")

    assert wake.contains(
        "if (diag & (DIAG_ENERGYOF_MASK | DIAG_CHARGEOF_MASK)) "
        "{ flags |= AUTO_FLAG_INA_ACCUM_OF; intervalValid = false; }"
    )
    assert wake.contains(
        "if (diag & DIAG_MATHOF_MASK) "
        "{ flags |= AUTO_FLAG_INA_MATHOF; intervalValid = false; }"
    )


def test_the_accumulators_are_still_read_before_they_are_reset():
    """D-020. The reorder moved DIAG_ALRT only; this must not have shifted."""

    wake = firmware_source.load().function("runAutonomousWakeCycle")

    charge = wake.index("readAccumulatedCharge_mAh(intervalCharge_mAh)")
    energy = wake.index("readAccumulatedEnergy_mWh(intervalEnergy_mWh)")
    reset = wake.index("resetInaAccumulators()")

    assert charge < reset
    assert energy < reset
