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

import shutil
import struct
import subprocess
import zlib
from pathlib import Path

import pytest

import firmware_source

from conftest import PROJECT_ROOT

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


# ============================================================================
# THE record_format MODULE BOUNDARY
# ============================================================================
#
# Added 2026-09-24, when the representation above moved out of solar-logger.ino
# into Arduino/solar-logger/record_format.{h,cpp}. Every test above this line is
# unchanged by that move, because firmware_source.py reads every sketch file as
# one. These tests are about the boundary itself: that the contract now has
# exactly one home, and that no policy or I/O followed it in.

RECORD_FORMAT_HEADER = PROJECT_ROOT / "Arduino" / "solar-logger" / "record_format.h"
RECORD_FORMAT_SOURCE = PROJECT_ROOT / "Arduino" / "solar-logger" / "record_format.cpp"

SKETCH = PROJECT_ROOT / "Arduino" / "solar-logger" / "solar-logger.ino"

# What record_format owns: the representation and its structural checks.
MODULE_FUNCTIONS = (
    ("autoRecordCrc", "uint32_t"),
    ("autoRecordValid", "bool"),
    ("autoRecordSnapshotUnread", "bool"),
)

# What deliberately did NOT move. printAutoRecord() interleaves flag
# interpretation with Serial formatting and comma bookkeeping for the LOGGER
# STORAGE DUMP transcript, which is an output format rather than the record
# format, so moving it would have been a formatting rewrite.
SKETCH_FUNCTIONS = ("printAutoRecord", "autoTimeQualityName")


def test_the_record_format_module_exists():
    assert RECORD_FORMAT_HEADER.is_file()
    assert RECORD_FORMAT_SOURCE.is_file()


@pytest.mark.parametrize("name,returns", MODULE_FUNCTIONS)
def test_the_record_helpers_live_in_the_module(name, returns):
    """Defined in record_format.cpp, and nowhere else.

    `firmware_source.definition()` raises on a second definition, so this also
    catches a copy left behind in the sketch.
    """

    found = firmware_source.load().definition(name, returns)

    assert found.tokens[0].where.startswith("Arduino/solar-logger/record_format.cpp")


@pytest.mark.parametrize("name", SKETCH_FUNCTIONS)
def test_the_printing_helpers_stayed_in_the_sketch(name):
    """The move stopped at the record format; the dump transcript did not go."""

    found = firmware_source.load()._only_definition(name)

    assert found.where.startswith("Arduino/solar-logger/solar-logger.ino")


def test_the_struct_and_its_constants_live_in_the_header():
    """One home for the deployed layout, and it is the header.

    Read as text rather than through the tokenizer, because the question is
    which FILE holds the contract.
    """

    header = RECORD_FORMAT_HEADER.read_text(encoding="utf-8")
    sketch = SKETCH.read_text(encoding="utf-8")

    assert "struct __attribute__((packed)) AutoRecord" in header
    assert "struct __attribute__((packed)) AutoRecord" not in sketch

    for constant in FROZEN_CONSTANTS:
        assert constant in header, f"not in record_format.h: {constant}"
        assert constant not in sketch, f"still defined in the sketch: {constant}"


@pytest.mark.parametrize("constant", FROZEN_CONSTANTS)
def test_each_frozen_constant_is_defined_exactly_once(constant):
    """Two definitions of AUTO_RECORD_MAGIC is how a format silently forks.

    A header constant is `constexpr` at namespace scope, so a second definition
    in another file links without complaint and changes what half the firmware
    believes.
    """

    assert firmware_source.load().code.count(constant) == 1


def test_the_module_holds_no_policy_state_or_io():
    """The boundary, asserted as an absence.

    record_format answers WHAT a record is and HOW its validity is checked. WHEN
    one is written, WHICH flags it carries and WHERE it is stored are storage and
    autonomous policy. Every name below belongs to one of those, and none of them
    may appear in this module's code.
    """

    module = firmware_source.load()

    for path in (RECORD_FORMAT_HEADER, RECORD_FORMAT_SOURCE):
        code = firmware_source.Code(
            firmware_source.tokenize(
                path.read_text(encoding="utf-8"), path.name
            )
        )

        for forbidden in (
            "LittleFS",       # storage mechanics
            "RTC_DATA_ATTR",  # retained autonomous state
            "Preferences",    # NVS
            "Serial",         # transcripts and telemetry
            "esp_sleep",      # the scheduler
            "millis",         # any clock at all
            "AUTO_SEQ_BLOCK", # sequence authority
            "AUTO_LOG_PATH",  # where records are kept
        ):
            assert not code.contains(forbidden), (
                f"{path.name} names `{forbidden}`; that is policy or I/O, and it "
                "belongs outside the record format"
            )

        # A flag's bit VALUE is intrinsic. Deciding that a given record carries
        # it is policy, and every `flags |=` in this firmware is such a decision.
        assert not code.contains("flags |="), (
            f"{path.name} sets a flag; the wake cycle decides which flags a "
            "record carries"
        )

    # Said positively too, so the test cannot pass by the module being empty.
    assert module.definition("autoRecordCrc", "uint32_t").contains(
        "esp_rom_crc32_le"
    )


@pytest.mark.parametrize("_kind,field,offset", FROZEN_LAYOUT)
def test_every_field_offset_is_asserted_at_compile_time(_kind, field, offset):
    """The compile-time guard is now per field, not just `sizeof == 72`.

    `sizeof(AutoRecord) == 72` cannot see two same-width fields swap places, a
    signed field turn unsigned, or a widened field paid for by a narrowed
    neighbor. All three keep the size at 72 and all three make Experiment 3's log
    unreadable. Verified by construction on 2026-09-24: flipping
    `avg_current_uA` to unsigned failed 3 assertions, and swapping `boot_id` with
    `session_elapsed_ms` failed 6, where the size assertion alone caught neither.
    """

    header = RECORD_FORMAT_HEADER.read_text(encoding="utf-8")
    width = WIDTH[_kind]

    assert f"AUTO_RECORD_FIELD_AT({field}, {offset}, {width});" in header

    signedness = "SIGNED" if _kind.startswith("int") else "UNSIGNED"

    assert f"AUTO_RECORD_FIELD_{signedness}({field});" in header


# ============================================================================
# EXECUTED: THE MODULE'S REAL SOURCE, COMPILED AND RUN ON THE HOST
# ============================================================================
#
# Everything above reads source or packs bytes in Python. These tests compile
# Arduino/solar-logger/record_format.cpp itself - the file the board is built
# from, not a copy - and run its `autoRecordCrc()` and `autoRecordValid()`
# against the real Experiment 3 record. `printAutoRecord()`, which stayed in the
# sketch, is extracted through firmware_source.py and compiled with it, so the
# whole path from 72 bytes to the transcript line is the firmware's own code.
#
# ONE SUBSTITUTION, AND WHAT LICENSES IT
# ======================================
#
# `esp_rom_crc32_le()` lives in the ESP32's mask ROM and cannot be linked on the
# host, so the harness supplies its own. That is the one place these tests are
# not running production code, and it would be worthless on its own: a stub
# compared against itself proves nothing.
#
# What makes it evidence is the golden record. The board stored 0xFB25DA73 for
# seq=4445, and `test_golden_record_reproduces_the_crc_the_board_stored` above
# already shows that `zlib.crc32` over the same 68 bytes produces that value.
# The stub below is asserted equal to `zlib.crc32` on those same bytes, so the
# chain is: stub == zlib == what the ROM actually did on the board.
#
# What the substitution therefore does NOT weaken: the struct's layout on a real
# compiler, the 68-byte covered range, the offset the CRC is read from, the
# magic and version checks, and every byte of the transcript line.

# The ROM function's seed is pre-inverted internally, which is why seeding with
# 0 gives the standard 0xFFFFFFFF initial value. Reflected polynomial 0xEDB88320,
# final inversion: IEEE 802.3 CRC-32.
CRC_SHIM_HEADER = """\
#pragma once

#include <stdint.h>

uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len);
"""

CRC_SHIM_SOURCE = """\
#include "esp_rom_crc.h"

uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len)
{
	crc = ~crc;

	while (len--)
	{
		crc ^= *buf++;

		for (int bit = 0; bit < 8; ++bit)
		{
			crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
		}
	}

	return ~crc;
}
"""

# Print::printNumber and Print::printFloat are copied from the ESP32 core so a
# value formats here exactly as it does on the board: uppercase hex with no
# leading zeros, and Arduino's add-half-then-truncate float rounding rather than
# printf's.
HARNESS_PRELUDE = """\
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "record_format.h"

// The golden bytes are little-endian because that is how the board wrote them.
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
							"this harness memcpy's the board's own bytes into AutoRecord");

constexpr int DEC = 10;
constexpr int HEX = 16;

struct RecordingSerial
{
	std::string text;

	// Print::printNumber, esp32 core 3.3.11, cores/esp32/Print.cpp.
	void printNumber(unsigned long n, uint8_t base)
	{
		char buf[8 * sizeof(long) + 1];
		char *str = &buf[sizeof(buf) - 1];
		*str = '\\0';

		if (base < 2) { base = 10; }

		do {
			char c = n % base;
			n /= base;
			*--str = c < 10 ? c + '0' : c + 'A' - 10;
		} while (n);

		text += str;
	}

	// Print::printFloat, esp32 core 3.3.11, cores/esp32/Print.cpp.
	void printFloat(double number, uint8_t digits)
	{
		if (std::isnan(number)) { text += "nan"; return; }
		if (std::isinf(number)) { text += "inf"; return; }
		if (number > 4294967040.0) { text += "ovf"; return; }
		if (number < -4294967040.0) { text += "ovf"; return; }

		if (number < 0.0)
		{
			text += '-';
			number = -number;
		}

		double rounding = 0.5;
		for (uint8_t i = 0; i < digits; ++i) { rounding /= 10.0; }
		number += rounding;

		unsigned long int_part = (unsigned long)number;
		double remainder = number - (double)int_part;
		text += std::to_string(int_part);

		if (digits > 0) { text += '.'; }

		while (digits-- > 0)
		{
			remainder *= 10.0;
			int toPrint = int(remainder);
			text += std::to_string(toPrint);
			remainder -= toPrint;
		}
	}

	void print(const char *value) { text += value; }
	void print(char value) { text += value; }

	void print(unsigned char value, int base) { printNumber(value, (uint8_t)base); }
	void print(unsigned int value, int base = DEC) { printNumber(value, (uint8_t)base); }
	void print(unsigned long value, int base = DEC) { printNumber(value, (uint8_t)base); }

	void print(int value, int base = DEC)
	{
		if (base == DEC && value < 0)
		{
			text += '-';
			printNumber((unsigned long)(-(long)value), (uint8_t)base);
			return;
		}

		printNumber((unsigned long)value, (uint8_t)base);
	}

	void print(long long value) { text += std::to_string(value); }
	void print(double value, int digits) { printFloat(value, (uint8_t)digits); }

	void println() { text += '\\n'; }
	void println(unsigned int value, int base = DEC) { print(value, base); text += '\\n'; }
};

static RecordingSerial Serial;
"""

HARNESS_DRIVER = r"""
static AutoRecord decoded(const char *hex)
{
	uint8_t bytes[AUTO_RECORD_SIZE];

	if (std::strlen(hex) != 2 * AUTO_RECORD_SIZE)
	{
		std::fprintf(stderr, "expected %zu hex characters, got %zu\n",
								 2 * AUTO_RECORD_SIZE, std::strlen(hex));
		std::exit(2);
	}

	for (size_t index = 0; index < AUTO_RECORD_SIZE; ++index)
	{
		unsigned value = 0;

		if (std::sscanf(hex + 2 * index, "%2x", &value) != 1)
		{
			std::fprintf(stderr, "bad hex at byte %zu\n", index);
			std::exit(2);
		}

		bytes[index] = (uint8_t)value;
	}

	AutoRecord record;
	std::memcpy(&record, bytes, AUTO_RECORD_SIZE);
	return record;
}

int main(int argc, char **argv)
{
	if (argc != 3)
	{
		std::fprintf(stderr, "usage: %s probe|print|mutations <144 hex chars>\n",
								 argv[0]);
		return 2;
	}

	const char *command = argv[1];
	AutoRecord record = decoded(argv[2]);

	if (std::strcmp(command, "probe") == 0)
	{
		std::printf("crc=%08X valid=%d snapshot=%d\n",
								autoRecordCrc(record),
								autoRecordValid(record) ? 1 : 0,
								autoRecordSnapshotUnread(record) ? 1 : 0);
		return 0;
	}

	if (std::strcmp(command, "print") == 0)
	{
		printAutoRecord(record);
		std::printf("%s", Serial.text.c_str());
		return 0;
	}

	if (std::strcmp(command, "mutations") == 0)
	{
		// Every byte of the record, flipped one at a time, through the real
		// validator. The unmutated record is index -1.
		std::printf("-1 %d\n", autoRecordValid(record) ? 1 : 0);

		for (size_t index = 0; index < AUTO_RECORD_SIZE; ++index)
		{
			uint8_t bytes[AUTO_RECORD_SIZE];
			std::memcpy(bytes, &record, AUTO_RECORD_SIZE);
			bytes[index] ^= 0xFF;

			AutoRecord mutated;
			std::memcpy(&mutated, bytes, AUTO_RECORD_SIZE);

			std::printf("%zu %d\n", index, autoRecordValid(mutated) ? 1 : 0);
		}

		return 0;
	}

	std::fprintf(stderr, "unknown command: %s\n", command);
	return 2;
}
"""


class RecordFormat:
    """The firmware's own record code, compiled for the host."""

    def __init__(self, binary: Path):
        self.binary = binary

    def _run(self, command: str, record: bytes) -> str:
        result = subprocess.run(
            [str(self.binary), command, record.hex()],
            capture_output=True,
            text=True,
            check=False,
        )

        if result.returncode != 0:
            raise RuntimeError(
                f"harness `{command}` exited {result.returncode}: {result.stderr}"
            )

        return result.stdout

    def probe(self, record: bytes) -> dict[str, int]:
        """The module's verdict on these 72 bytes: CRC, validity, snapshot."""

        fields = self._run("probe", record).strip().split()

        return {
            "crc": int(fields[0].removeprefix("crc="), 16),
            "valid": int(fields[1].removeprefix("valid=")),
            "snapshot": int(fields[2].removeprefix("snapshot=")),
        }

    def printed(self, record: bytes) -> str:
        """Exactly what printAutoRecord() emits, newline stripped."""

        return self._run("print", record).rstrip("\n")

    def mutations(self, record: bytes) -> dict[int, bool]:
        """Validity after flipping each byte in turn; index -1 is untouched."""

        return {
            int(index): verdict == "1"
            for index, verdict in (
                line.split() for line in self._run("mutations", record).splitlines()
            )
        }


@pytest.fixture(scope="module")
def record_format(tmp_path_factory: pytest.TempPathFactory) -> RecordFormat:
    compiler = shutil.which("c++")

    if compiler is None:
        pytest.fail(
            "No host C++ compiler (`c++`) on PATH. These tests compile the "
            "firmware's own record_format.cpp and run it against the deployed "
            "Experiment 3 record; without a compiler the binary contract has no "
            "executed cover, so this is a failure, not a skip."
        )

    build = tmp_path_factory.mktemp("record_format")
    (build / "esp_rom_crc.h").write_text(CRC_SHIM_HEADER, encoding="utf-8")
    (build / "crc_shim.cpp").write_text(CRC_SHIM_SOURCE, encoding="utf-8")

    sketch = firmware_source.load()
    extracted = "\n\n".join(
        str(sketch.definition(name, returns))
        for name, returns in (
            ("autoTimeQualityName", "const char *"),
            ("printAutoRecord", "void"),
        )
    )
    harness = "\n\n".join([HARNESS_PRELUDE, extracted, HARNESS_DRIVER])
    (build / "harness.cpp").write_text(harness, encoding="utf-8")

    binary = build / "record_harness"
    result = subprocess.run(
        [
            compiler,
            "-std=c++20",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{build}",
            f"-I{RECORD_FORMAT_HEADER.parent}",
            "-o",
            str(binary),
            str(build / "harness.cpp"),
            str(RECORD_FORMAT_SOURCE),
            str(build / "crc_shim.cpp"),
        ],
        capture_output=True,
        text=True,
        check=False,
    )

    if result.returncode != 0:
        pytest.fail(
            "The firmware's record code did not compile on the host:\n"
            f"{result.stderr}\n--- harness source ---\n{harness}"
        )

    return RecordFormat(binary)


def test_the_crc_shim_really_is_the_convention_the_board_used():
    """Guard the one substitution, so it cannot drift into proving nothing.

    Not a property of the firmware: a property of this harness. If it ever fails,
    every executed test below is measuring the wrong CRC.
    """

    # Asserted against the value the BOARD stored, not against the shim's own
    # output, so this cannot pass by both sides being wrong together.
    assert auto_record_crc(golden_covered_bytes()) == GOLDEN_CRC


def test_the_module_computes_the_crc_the_board_stored(record_format: RecordFormat):
    """autoRecordCrc(), compiled and run, on the real Experiment 3 record.

    The strongest executed statement here. The struct is the firmware's, laid out
    by a real compiler; the covered range and the seed are the firmware's; and
    the expected number came off the board.
    """

    assert record_format.probe(golden_record_bytes())["crc"] == GOLDEN_CRC


def test_the_module_accepts_the_real_deployed_record(record_format: RecordFormat):
    probe = record_format.probe(golden_record_bytes())

    assert probe["valid"] == 1
    assert probe["snapshot"] == 0, "seq=4445 carries four real measurements"


def test_the_module_rejects_a_wrong_magic_even_with_a_correct_crc(
    record_format: RecordFormat,
):
    """The magic check is real, not an accident of the CRC failing anyway.

    Corrupting magic normally breaks the CRC too, which would let a firmware that
    had dropped the magic check pass. So the CRC is recomputed over the corrupted
    bytes - by the module itself - before validity is asked.
    """

    corrupted = bytearray(golden_covered_bytes())
    struct.pack_into("<H", corrupted, 0, 0x1234)
    resealed = bytes(corrupted) + struct.pack("<I", 0)

    crc = record_format.probe(resealed)["crc"]
    resealed = bytes(corrupted) + struct.pack("<I", crc)

    assert record_format.probe(resealed)["valid"] == 0


def test_the_module_rejects_a_wrong_version_even_with_a_correct_crc(
    record_format: RecordFormat,
):
    """Version 2 bytes are not version 1 bytes, and must not be read as them."""

    corrupted = bytearray(golden_covered_bytes())
    corrupted[2] = 2
    resealed = bytes(corrupted) + struct.pack("<I", 0)

    crc = record_format.probe(resealed)["crc"]
    resealed = bytes(corrupted) + struct.pack("<I", crc)

    assert record_format.probe(resealed)["valid"] == 0


def test_the_module_rejects_every_single_byte_corruption(
    record_format: RecordFormat,
):
    """All 72 bytes, flipped one at a time, through the real validator.

    The Python mutation tests above prove the CRC covers bytes 0-67. This proves
    the firmware's own `autoRecordValid()` acts on that: 68 covered bytes fail
    their checksum, and the 4 CRC bytes fail by no longer matching it.
    """

    verdicts = record_format.mutations(golden_record_bytes())

    assert verdicts.pop(-1) is True, "the untouched record must validate"
    assert len(verdicts) == 72

    rejected = [index for index, valid in verdicts.items() if not valid]

    assert sorted(rejected) == list(range(72))


def test_the_module_recognizes_the_snapshot_unread_sentinels(
    record_format: RecordFormat,
):
    """The four impossible values, written together, read back as one condition."""

    unread = bytearray(golden_covered_bytes())
    struct.pack_into("<I", unread, 44, 0xFFFFFFFF)          # bus_uV
    for offset in (48, 52, 56):                             # current, power, temp
        struct.pack_into("<i", unread, offset, -(2**31))

    sealed = bytes(unread) + struct.pack("<I", 0)
    crc = record_format.probe(sealed)["crc"]
    sealed = bytes(unread) + struct.pack("<I", crc)

    probe = record_format.probe(sealed)

    assert probe["valid"] == 1, "a failed snapshot still stores a valid record"
    assert probe["snapshot"] == 1


# ============================================================================
# printAutoRecord: THE TRANSCRIPT LINE, UNCHANGED BY THE MOVE
# ============================================================================

# The tail of the verbatim LOGGER STORAGE DUMP line quoted beside GOLDEN_FIELDS,
# from `seq=` onwards. The `[STORAGE] #3033 ` prefix is printed by the dump loop
# in solar-logger.ino, not by printAutoRecord().
#
# This is evidence, not a fixture chosen to pass: the board emitted it. It is
# what "printAutoRecord behavior is unchanged" means as bytes, and it is the one
# consumer that reads the moved flag constants and calls the moved helper.
GOLDEN_DUMP_TAIL = (
    "seq=4445 boot=26 exp=3 elapsed_ms=58629900 interval_ms=60004 "
    "V=13.051367 I_mA=-0.616 P_mW=8.025 T_C=18.695 "
    "dQ_uAh=-10 dE_uWh=129 Qsum_uAh=380771 Esum_uWh=5602351 "
    "time=UNKNOWN epoch=0 flags=0x0[none] crc=0xFB25DA73"
)


def test_print_auto_record_reproduces_the_line_the_board_printed(
    record_format: RecordFormat,
):
    """72 real bytes in, the real transcript line out."""

    assert record_format.printed(golden_record_bytes()) == GOLDEN_DUMP_TAIL


def test_print_auto_record_decodes_every_flag_name(record_format: RecordFormat):
    """All six named bits, and the comma bookkeeping between them.

    The flag NAMES and their order are the part of the dump a person reads. This
    is the test that a move of the bit values cannot quietly renumber them: the
    names come from printAutoRecord() in the sketch, the values from
    record_format.h, and a mismatch shows up as a wrong name here.
    """

    every_flag = bytearray(golden_covered_bytes())
    every_flag[3] = 0xFC  # every bit but the two time-quality bits
    sealed = bytes(every_flag) + struct.pack("<I", 0)
    crc = record_format.probe(sealed)["crc"]
    sealed = bytes(every_flag) + struct.pack("<I", crc)

    printed = record_format.printed(sealed)

    assert "flags=0xFC[FIRST_AFTER_BOOT,ACCUM_SUSPECT,INTERVAL_ODD," \
           "INA_MATHOF,INA_ACCUM_OF,EXPERIMENT_UNKNOWN]" in printed

    # Bit 7 set means the experiment id is not to be trusted, and the dump says
    # so by name instead of printing UINT32_MAX.
    assert " exp=UNKNOWN " in printed


@pytest.mark.parametrize(
    "quality,name",
    ((0, "UNKNOWN"), (1, "SYNCHRONIZED"), (2, "HOLDOVER"), (3, "RESERVED")),
)
def test_print_auto_record_names_each_time_quality(
    quality, name, record_format: RecordFormat
):
    """The two-bit field, including the value no record should carry.

    3 is unassigned. It prints RESERVED rather than falling through to UNKNOWN,
    so a record that somehow carries it cannot be read as "the clock was never
    set" - which is a different and weaker claim.
    """

    record = bytearray(golden_covered_bytes())
    record[3] = quality
    sealed = bytes(record) + struct.pack("<I", 0)
    crc = record_format.probe(sealed)["crc"]
    sealed = bytes(record) + struct.pack("<I", crc)

    assert f" time={name} " in record_format.printed(sealed)


def test_print_auto_record_names_an_unread_snapshot(record_format: RecordFormat):
    """4294.97 V is obviously wrong; SNAPSHOT_UNREAD says which wrong."""

    unread = bytearray(golden_covered_bytes())
    struct.pack_into("<I", unread, 44, 0xFFFFFFFF)
    for offset in (48, 52, 56):
        struct.pack_into("<i", unread, offset, -(2**31))

    sealed = bytes(unread) + struct.pack("<I", 0)
    crc = record_format.probe(sealed)["crc"]
    sealed = bytes(unread) + struct.pack("<I", crc)

    printed = record_format.printed(sealed)

    assert (
        " V=SNAPSHOT_UNREAD I_mA=SNAPSHOT_UNREAD"
        " P_mW=SNAPSHOT_UNREAD T_C=SNAPSHOT_UNREAD" in printed
    )
    assert "4294.967295" not in printed
