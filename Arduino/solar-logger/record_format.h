// ============================================================================
// RECORD FORMAT
// ============================================================================
//
// WHAT a durable autonomous record is, and HOW its structural validity is
// checked. Nothing else. This module owns the version-1 on-flash
// representation: the 72 packed bytes, the magic and version that identify
// them, the flag bit values and the impossible sentinels that give some of
// those bytes their meaning, and the CRC that seals them.
//
// It decides nothing about records. It does not know when one is created, which
// values or flags belong in it, where it is stored, how a log is scanned or
// recovered, what a sequence number means, or that deep sleep exists. Those are
// storage and autonomous policy, they stay in solar-logger.ino, and they call
// down into this module (D-047).
//
// THE BIT VALUES ARE INTRINSIC; SETTING THEM IS POLICY. AUTO_FLAG_INA_ACCUM_OF
// is 0x40 because every stored record says so, and that fact belongs here.
// Whether a particular record gets that bit is decided by the wake cycle, which
// reads DIAG_ALRT and judges the interval. No flag-setting rule, and no
// time-quality state machine, lives in this module.
//
// THE LAYOUT IS A DEPLOYED BINARY CONTRACT, NOT A CHOICE. Experiment 3's log
// holds thousands of records written by exactly this layout, and the next image
// has to keep reading them. Every field's offset, width, signedness and byte
// order, the covered CRC range, the CRC offset, and the magic and version are
// frozen. Changing any of them is a record version decision, not a patch.
//
// It is protected three ways, deliberately overlapping:
//
//   compile time   the static_asserts below fail the build on a size change,
//                  a reordered field, a changed width, or a flipped signedness
//   host tests     tests/test_characterization_record.py rebuilds a REAL
//                  Experiment 3 record byte for byte and reproduces the CRC the
//                  board itself stored (seq=4445, crc=0xFB25DA73)
//   source tests   the frozen constants and the helper bodies below are pinned
//                  as tokens, so a move cannot quietly rewrite one
//
// WHAT IS NOT HERE. printAutoRecord() stays in solar-logger.ino. It interleaves
// flag interpretation with Serial formatting and comma bookkeeping for the
// LOGGER STORAGE DUMP transcript, which is an output format rather than the
// record format, and it reads the constants above through this header.
// ============================================================================

#pragma once

// A .cpp does not get the <Arduino.h> that Arduino CLI prepends to the sketch,
// so this header includes what it uses: the fixed-width types and their limits,
// and size_t/offsetof for the layout assertions (D-047).
#include <stddef.h>
#include <stdint.h>

// ----------------------------------------------------------------------------
// RECORD IDENTITY
// ----------------------------------------------------------------------------

constexpr uint16_t AUTO_RECORD_MAGIC = 0xB5A5;
constexpr uint8_t AUTO_RECORD_VERSION = 1;

// ----------------------------------------------------------------------------
// RECORD FLAG BITS
// ----------------------------------------------------------------------------
//
// The bit values only. Which of them a given record carries is decided by the
// autonomous wake cycle in solar-logger.ino.

// Record flag bits. Layout from docs/STORAGE_SYNC_DESIGN.md Section 5.
constexpr uint8_t AUTO_FLAG_TIME_QUALITY_MASK = 0x03;
constexpr uint8_t AUTO_TIME_UNKNOWN = 0;
constexpr uint8_t AUTO_TIME_SYNCHRONIZED = 1;
constexpr uint8_t AUTO_TIME_HOLDOVER = 2;

constexpr uint8_t AUTO_FLAG_FIRST_AFTER_BOOT = 0x04;
constexpr uint8_t AUTO_FLAG_ACCUM_SUSPECT = 0x08;
constexpr uint8_t AUTO_FLAG_INTERVAL_ODD = 0x10;
constexpr uint8_t AUTO_FLAG_INA_MATHOF = 0x20;
constexpr uint8_t AUTO_FLAG_INA_ACCUM_OF = 0x40;

// Bit 7 was proposed as SEQ_GAP. It was never implemented and never set, and a
// sequence gap is already directly visible by comparing consecutive seq values
// in the log, so the bit was redundant. It now carries something that cannot be
// recovered any other way: whether the experiment attribution is trustworthy.
//
// No stored record has bit 7 set, so no existing record changes meaning. See
// docs/STORAGE_SYNC_DESIGN.md Section 5.
constexpr uint8_t AUTO_FLAG_EXPERIMENT_UNKNOWN = 0x80;

// Written into experiment_id when the id could not be determined. The FLAG is
// authoritative; this sentinel exists so a record that slips past a host's flag
// check still cannot be silently counted as experiment 0.
constexpr uint32_t AUTO_EXPERIMENT_UNKNOWN = UINT32_MAX;

// ----------------------------------------------------------------------------
// SNAPSHOT-UNREAD SENTINELS
// ----------------------------------------------------------------------------
//
// Written into the four instantaneous snapshot fields when readSensor() fails
// during a wake. The interval CHARGE and ENERGY are read earlier and separately,
// so a failed snapshot does not invalidate them: the record is still stored,
// because discarding a good interval integral to avoid reporting a bad
// instantaneous sample would lose the more valuable half.
//
// WHY A SENTINEL AND NOT A FLAG BIT. All eight bits of `flags` are assigned
// (docs/STORAGE_SYNC_DESIGN.md Section 5) and the 72-byte layout is frozen, so
// there is no bit available for a ninth condition and adding one is a record
// schema change that needs a decision rather than a patch. These values are
// chosen to be physically impossible instead:
//
//   bus_uV        UINT32_MAX = 4294.97 V   against an 85 V part maximum
//   avg_current   INT32_MIN  = -2147 A
//   avg_power     INT32_MIN  = -2147 kW
//   temp_mC       INT32_MIN  = -2147483 C
//
// No real reading can produce any of them, so a host cannot mistake one for a
// measurement. LOGGER STORAGE DUMP decodes them by name as SNAPSHOT_UNREAD, so
// the condition is visible without the table.
//
// Reusing AUTO_FLAG_ACCUM_SUSPECT here was considered and rejected: it means
// "the accumulators may not cover the full interval", which is a different and
// in this case untrue claim.
constexpr uint32_t AUTO_SNAPSHOT_BUS_UV_UNREAD = UINT32_MAX;
constexpr int32_t AUTO_SNAPSHOT_SIGNED_UNREAD = INT32_MIN;

// ----------------------------------------------------------------------------
// THE DURABLE RECORD
// ----------------------------------------------------------------------------

// The durable record. Field order and offsets are exactly as specified in
// docs/STORAGE_SYNC_DESIGN.md Section 5: every 4-byte field is 4-aligned and
// both 64-bit fields are 8-aligned, so the packed layout needs no padding.
struct __attribute__((packed)) AutoRecord
{
	uint16_t magic;							 // 0
	uint8_t version;						 // 2
	uint8_t flags;							 // 3
	uint32_t seq;								 // 4
	int64_t running_charge_uAh;	 // 8   TEST-LOCAL, not experiment totals
	int64_t running_energy_uWh;	 // 16  TEST-LOCAL, not experiment totals
	uint32_t experiment_id;			 // 24  read-only context
	uint32_t boot_id;						 // 28
	uint32_t session_elapsed_ms; // 32
	uint32_t epoch_s;						 // 36
	uint32_t interval_ms;				 // 40
	uint32_t bus_uV;						 // 44
	int32_t avg_current_uA;			 // 48
	int32_t avg_power_uW;				 // 52
	int32_t temp_mC;						 // 56
	int32_t interval_charge_uAh; // 60
	int32_t interval_energy_uWh; // 64
	uint32_t crc32;							 // 68
}; // 72

// If this ever fails, the on-disk format has silently changed and every
// stored record becomes unreadable. Fail at compile time instead.
static_assert(sizeof(AutoRecord) == 72,
							"AutoRecord must be exactly 72 bytes; see STORAGE_SYNC_DESIGN.md");

constexpr size_t AUTO_RECORD_SIZE = sizeof(AutoRecord);

// ----------------------------------------------------------------------------
// FIELD-BY-FIELD LAYOUT ASSERTIONS
// ----------------------------------------------------------------------------
//
// The size assertion above catches a record that grew or shrank. It does NOT
// catch two same-width fields swapping places, a signed field becoming
// unsigned, or a widened field paid for by a narrowed neighbor: all three keep
// sizeof() at 72 and all three make every stored record unreadable.
//
// So each field's offset and width is asserted by name, and each field's
// signedness with it. The offsets are the ones in the comments above, which are
// now checked rather than trusted.
//
// -1 converted to the field's own type is negative only if that type is signed.
// It needs no <type_traits>, and it says which way each field has to go.

#define AUTO_RECORD_FIELD_AT(field, offset, width)                         \
	static_assert(offsetof(AutoRecord, field) == (offset),                   \
								#field " must sit at byte " #offset "; the on-flash "      \
											 "layout is frozen");                               \
	static_assert(sizeof(AutoRecord::field) == (width),                      \
								#field " must be " #width " bytes wide")

AUTO_RECORD_FIELD_AT(magic, 0, 2);
AUTO_RECORD_FIELD_AT(version, 2, 1);
AUTO_RECORD_FIELD_AT(flags, 3, 1);
AUTO_RECORD_FIELD_AT(seq, 4, 4);
AUTO_RECORD_FIELD_AT(running_charge_uAh, 8, 8);
AUTO_RECORD_FIELD_AT(running_energy_uWh, 16, 8);
AUTO_RECORD_FIELD_AT(experiment_id, 24, 4);
AUTO_RECORD_FIELD_AT(boot_id, 28, 4);
AUTO_RECORD_FIELD_AT(session_elapsed_ms, 32, 4);
AUTO_RECORD_FIELD_AT(epoch_s, 36, 4);
AUTO_RECORD_FIELD_AT(interval_ms, 40, 4);
AUTO_RECORD_FIELD_AT(bus_uV, 44, 4);
AUTO_RECORD_FIELD_AT(avg_current_uA, 48, 4);
AUTO_RECORD_FIELD_AT(avg_power_uW, 52, 4);
AUTO_RECORD_FIELD_AT(temp_mC, 56, 4);
AUTO_RECORD_FIELD_AT(interval_charge_uAh, 60, 4);
AUTO_RECORD_FIELD_AT(interval_energy_uWh, 64, 4);
AUTO_RECORD_FIELD_AT(crc32, 68, 4);

#undef AUTO_RECORD_FIELD_AT

#define AUTO_RECORD_FIELD_SIGNED(field)                                    \
	static_assert(static_cast<decltype(AutoRecord::field)>(-1) < 0,           \
								#field " must be signed; real records carry negative "     \
											 "values in it")

#define AUTO_RECORD_FIELD_UNSIGNED(field)                                  \
	static_assert(static_cast<decltype(AutoRecord::field)>(-1) > 0,           \
								#field " must be unsigned; a host reads it as a "          \
											 "magnitude")

AUTO_RECORD_FIELD_UNSIGNED(magic);
AUTO_RECORD_FIELD_UNSIGNED(version);
AUTO_RECORD_FIELD_UNSIGNED(flags);
AUTO_RECORD_FIELD_UNSIGNED(seq);
AUTO_RECORD_FIELD_SIGNED(running_charge_uAh);
AUTO_RECORD_FIELD_SIGNED(running_energy_uWh);
AUTO_RECORD_FIELD_UNSIGNED(experiment_id);
AUTO_RECORD_FIELD_UNSIGNED(boot_id);
AUTO_RECORD_FIELD_UNSIGNED(session_elapsed_ms);
AUTO_RECORD_FIELD_UNSIGNED(epoch_s);
AUTO_RECORD_FIELD_UNSIGNED(interval_ms);
AUTO_RECORD_FIELD_UNSIGNED(bus_uV);
AUTO_RECORD_FIELD_SIGNED(avg_current_uA);
AUTO_RECORD_FIELD_SIGNED(avg_power_uW);
AUTO_RECORD_FIELD_SIGNED(temp_mC);
AUTO_RECORD_FIELD_SIGNED(interval_charge_uAh);
AUTO_RECORD_FIELD_SIGNED(interval_energy_uWh);
AUTO_RECORD_FIELD_UNSIGNED(crc32);

#undef AUTO_RECORD_FIELD_SIGNED
#undef AUTO_RECORD_FIELD_UNSIGNED

// The CRC is the last field, so the covered range is everything before it.
// autoRecordCrc() computes that length as AUTO_RECORD_SIZE - sizeof(uint32_t);
// this is the same 68 bytes said the other way, so a future field appended
// after crc32 fails here instead of silently leaving itself out of the seal.
static_assert(offsetof(AutoRecord, crc32) + sizeof(AutoRecord::crc32) ==
									AUTO_RECORD_SIZE,
							"crc32 must be the last field: everything before it is sealed, "
							"and nothing after it would be");

// ----------------------------------------------------------------------------
// CRC AND STRUCTURAL VALIDITY
// ----------------------------------------------------------------------------

// CRC-32 over bytes 0..67, i.e. every byte except the trailing crc32 field.
//
// esp_rom_crc32_le(0, ...) is standard reflected IEEE 802.3 CRC-32, the same
// value zlib.crc32 produces. That is not asserted from the name: the deployed
// Experiment 3 record in tests/test_characterization_record.py reproduces
// 0xFB25DA73 from its own 68 bytes, which is what proves the convention.
uint32_t autoRecordCrc(const AutoRecord &record);

// Whether these 72 bytes are a record this firmware wrote: right magic, right
// version, and a CRC that matches the bytes it seals.
//
// STRUCTURAL ONLY. It says nothing about whether the values are plausible,
// whether the sequence follows the previous record, or whether the record
// belongs to the current experiment. Those are the caller's questions.
bool autoRecordValid(const AutoRecord &record);

// True when this record's wake could not read the instantaneous snapshot, which
// the four impossible sentinels above record.
bool autoRecordSnapshotUnread(const AutoRecord &record);
