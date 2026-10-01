// ============================================================================
// SEQUENCE AUTHORITY - implementation
// ============================================================================
//
// Moved out of autoStorageRecover() in solar-logger.ino on 2026-10-01
// unchanged, apart from the reservation arriving as a parameter where the
// function used to read the RTC mirror it had just loaded. See
// sequence_authority.h for what the module owns and what it leaves behind.
//
// A .cpp is compiled on its own, without the #include <Arduino.h> that Arduino
// CLI prepends to the sketch, so it includes Arduino.h itself for Serial.
// ============================================================================

#include "sequence_authority.h"

#include <Arduino.h>

uint32_t sequenceAuthorityNext(const AutoLogScan &scan,
															 uint32_t reservedHighWater)
{
	// Where the next sequence comes from.
	//
	// The log is the only thing that ever writes a record, so when the log is
	// provably intact it is also the authority on the highest sequence ever
	// used, and the next record can continue directly from its tail. Skipping
	// past the NVS reservation in that case produced a large gap on every clean
	// restart for no safety benefit.
	//
	// The NVS reservation is the floor in every other case, because then the log
	// tail cannot be trusted to show everything that was written:
	//
	//   - an empty log may have been cleared after records were already synced
	//   - a partial or corrupt tail means records were lost
	//   - out-of-order sequences mean the log already contains a reuse
	//
	// Gaps stay acceptable. Duplicates stay forbidden.
	//
	// NOTE for when pruning is implemented: a pruned log no longer holds the
	// highest sequence ever used, so "log is intact" will stop being sufficient
	// and the NVS floor will have to apply unconditionally again.
	//
	// `invalidRecords == 0` now covers mid-file damage as well as a bad tail,
	// because the scan no longer stops at the first bad record. A log with a
	// preserved mid-file corruption is therefore NOT intact, and the NVS floor
	// applies to it, which is the conservative side.
	const bool logIntact = scan.fileExists && scan.validRecords > 0 &&
												 !scan.partialTail && !scan.seqOutOfOrder &&
												 !scan.readError && scan.invalidRecords == 0 &&
												 scan.trailingBytes == 0;

	uint32_t fromLog = (scan.validRecords > 0) ? scan.lastSeq + 1 : 1;

	// One past the reservation, not at it. The reservation covers up to
	// highWater - 1, so this deliberately skips one extra number rather than
	// risk landing on a sequence that a lost record might have used.
	uint32_t fromNvs = reservedHighWater + 1;

	uint32_t next = logIntact ? fromLog
														: ((fromLog > fromNvs) ? fromLog : fromNvs);

	Serial.print("[STORAGE] Next sequence from log: ");
	Serial.print(fromLog);
	Serial.print(", from NVS reservation: ");
	Serial.print(fromNvs);
	Serial.print(", using: ");
	Serial.println(next);

	if (logIntact)
	{
		Serial.println(
				"[STORAGE] Log tail is provably intact, so it is the authority and "
				"the NVS reservation floor was not applied.");
	}
	else
	{
		Serial.println(
				"[STORAGE] Log tail is NOT provably intact, so the NVS reservation "
				"floor applies.");
	}

	if (next > fromLog)
	{
		Serial.println(
				"[STORAGE] A sequence gap was skipped. Gaps are expected after a "
				"crash and are preferable to reuse.");
	}

	return next;
}
