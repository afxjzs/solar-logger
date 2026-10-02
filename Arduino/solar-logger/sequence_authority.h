// ============================================================================
// SEQUENCE AUTHORITY
// ============================================================================
//
// WHICH sequence number the next durable record gets, decided by reconciling
// what the log says against what the NVS reservation floor says (D-023).
// Nothing else.
//
// It is handed both facts and reads neither. The scan comes from
// autoStorageScanAndRepair() in storage.h, which reports the log and decides
// nothing about sequences (D-058). The reservation high-water mark is read out
// of NVS by the caller, into the RTC-retained mirror the caller maintains, and
// passed in by value. So this module names no Preferences, no LittleFS, no
// RTC_DATA_ATTR variable and no `extern`: every dependency is a parameter
// (D-050), and it calls nothing but Serial.
//
// WHAT IS NOT HERE. Reserving a block, the block width, and the writes to the
// RTC mirror of the reservation stay in solar-logger.ino: reserveSequenceBlock()
// and ensureSequenceReservation() write rtcAutoSeqHighWater, which is
// RTC-retained autonomous state. Its definition and lifetime belong to
// autonomous_retained_state (D-063); reservation policy, which owns its writes,
// is a later stage. Reseeding the RTC running
// totals stays in solar-logger.ino's autoStorageRecover() too (D-060, D-061);
// it shares a caller with this decision, not an owner.
//
// The decision is deployed behavior, preserved exactly by the 2026-10-01 move:
// tests/test_characterization_storage_recovery.py executes it on the host,
// through autoStorageRecover(), for an intact, partial, corrupt, unreadable and
// empty log.
// ============================================================================

#pragma once

#include <stdint.h>

#include "storage.h"

// The next sequence to use, decided from a completed boot-recovery scan and
// the NVS reservation high-water mark. Prints which of the two it used and why
// as [STORAGE] lines.
//
// An intact log tail is the authority. Otherwise the floor applies, and the
// result is never below one past the reservation.
uint32_t sequenceAuthorityNext(const AutoLogScan &scan,
															 uint32_t reservedHighWater);
