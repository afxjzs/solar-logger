// ============================================================================
// DURABLE STORAGE
// ============================================================================
//
// WHERE durable autonomous records are kept, and HOW the flash holding them is
// mounted, read, appended to, scanned and repaired. Nothing else. This module
// owns the LittleFS mechanics: the log path, the temporary path a repair
// writes through, the mount that never formats, the one pass that classifies
// every record in the file, the tail repair, and the append.
//
// It decides nothing about records. WHAT a record is, which bytes it occupies
// and whether a given 72 bytes are structurally valid belong to
// record_format.h, which this module calls down into (D-047, D-056).
//
// IT ALSO DECIDES NOTHING ABOUT SEQUENCES. autoStorageScanAndRepair() reports
// what the log holds; which sequence number the next record gets is the D-023
// reconciliation of an intact log tail against the NVS reservation floor, and
// that decision, the NVS read behind it and the RTC-retained state it reseeds
// all stay in solar-logger.ino's autoStorageRecover(). This module names no
// Preferences, no RTC_DATA_ATTR variable, no scheduler and no command parser
// (D-055).
//
// STORAGE-FULL POLICY IS NOT HERE, because there is not one yet. Free space is
// reported; what to do when it runs out is undecided, and inventing an answer
// inside a structural move is exactly the kind of silent deviation this
// project's rules forbid. Sync, storage ACKs and reclamation are likewise
// absent rather than stubbed.
//
// THE RECOVERY CONTRACT IS DEPLOYED BEHAVIOR, NOT A CHOICE. D-057 settles it:
// a torn tail and an invalid final record are repaired automatically; mid-file
// damage and a read that comes up short are REPORTED and the file is left
// exactly as it was found. tests/test_characterization_storage_recovery.py
// compiles the functions below and runs them against damaged logs made of real
// files, so the refusals are executed rather than reviewed.
//
// WHAT IS NOT HERE. printAutoRecord(), dumpStorage(), printStorageInfo() and
// clearStorage() stay in solar-logger.ino. Each one interleaves operator-facing
// formatting, or the autonomous test's own state, with the file access it
// needs; only the file access moved, which is why this header exports
// autoStorageOpenLogForRead(), autoStorageLogExists(), autoStorageTotalBytes(),
// autoStorageUsedBytes() and autoStorageFormat() rather than those commands.
// ============================================================================

#pragma once

// A .cpp does not get the <Arduino.h> that Arduino CLI prepends to the sketch,
// so this header includes what it uses (D-047): the fixed-width types, size_t,
// the record contract, and <FS.h> for File, which autoStorageOpenLogForRead()
// hands back to LOGGER STORAGE DUMP.
#include <stddef.h>
#include <stdint.h>

#include <FS.h>

#include "record_format.h"

// ----------------------------------------------------------------------------
// WHAT ONE FULL SCAN FOUND
// ----------------------------------------------------------------------------
//
// THE DAMAGE CASES ARE NOT ONE CASE. A torn tail (leftover bytes), an invalid
// record at the end, an invalid record with good records after it, and a log
// that cannot be read to the end are different problems with different safe
// answers, so they are counted separately here rather than collapsed into
// "the log is bad".
//
//   trailingBytes              what an automatic repair would DISCARD: the
//                              contiguous run of invalid records at the end,
//                              plus any partial-record remainder
//   keepBytes                  what it would KEEP, always a whole number of
//                              records and always entirely valid ones
//   validAfterInvalidRecords   good records stranded PAST damage. Non-zero
//                              means no automatic repair may run at all: see
//                              autoStorageScanAndRepair().
struct AutoLogScan
{
	bool mounted;
	bool fileExists;
	uint32_t validRecords;
	uint32_t firstSeq;
	uint32_t lastSeq;
	size_t fileBytes;
	size_t validBytes;                 // bytes held by valid records, anywhere
	size_t trailingBytes;              // bytes an automatic repair would discard
	size_t keepBytes;                  // bytes an automatic repair would keep
	size_t firstInvalidOffset;         // byte offset of the first bad record
	uint32_t invalidRecords;           // whole-sized records that failed
	uint32_t trailingInvalidRecords;   // of those, the unbroken run at the end
	uint32_t validAfterInvalidRecords; // valid records past the first bad one
	bool partialTail;                  // size is not a whole number of records
	bool readError;                    // the log could not be read to the end
	bool seqOutOfOrder;
	int64_t lastRunChargeUAh;
	int64_t lastRunEnergyUWh;
};

// Sub-timings from the last autoStorageAppend(), in microseconds. Split out
// because open, write, and flush+close have very different costs on LittleFS
// and a single combined number hides which one dominates the wake.
//
// Read through autoStorageLastAppendTiming() rather than exported as three
// mutable globals: the wake cycle prints them, and only the append writes them.
struct AutoAppendTiming
{
	uint32_t openUs;
	uint32_t writeUs;
	uint32_t flushCloseUs;
};

// ----------------------------------------------------------------------------
// MOUNTING
// ----------------------------------------------------------------------------

// Mount the stock "spiffs" partition, and NEVER format it on failure.
// Formatting would destroy stored history, and a first-run unformatted
// partition is indistinguishable from a corrupted one at mount time.
// Initializing is an explicit operator action: LOGGER STORAGE CLEAR YES.
//
// Idempotent: a second call while mounted returns true without touching the
// filesystem. `verbose` prints the operator explanation on failure; the
// autonomous wake passes false because nothing is draining the port.
bool autoStorageMount(bool verbose);

// Format the partition and drop the mounted flag, for LOGGER STORAGE CLEAR YES.
// It is the caller that decides a clear is allowed; this only performs it.
// Returns false, having said why, if the format failed.
bool autoStorageFormat();

// ----------------------------------------------------------------------------
// READING THE LOG
// ----------------------------------------------------------------------------

// One pass over the whole log, classifying every 72-byte slot. Reports; never
// modifies. Safe to call at any time.
AutoLogScan autoStorageScan();

// Boot recovery, as far as the FILE is concerned: scan, report every damage
// case by name, and repair the log only when the damage is confined to its
// tail. Returns the scan, so the caller can derive the next sequence from it
// without reading the file a second time.
//
// A repair that ran and a repair that was refused never read alike. The
// sequence decision is deliberately NOT here; see autoStorageRecover() in
// solar-logger.ino.
AutoLogScan autoStorageScanAndRepair();

// Rewrite the log to exactly its valid prefix, discarding only tail damage, and
// REFUSE when the damage is anywhere else. This is the only code in the
// firmware that deletes stored records.
//
// Declared here although autoStorageScanAndRepair() is its only caller, which
// is a departure from D-047's "export only what is used outside". The reason is
// measured, not stylistic: given internal linkage and one call site, GCC proves
// the refusal unreachable from that caller's `else if` chain and deletes it
// from the image. The refusal is a property of this function under D-057, not
// of whoever calls it, so it stays reachable and stays compiled.
bool autoStorageTruncateToValid(const AutoLogScan &scan);

// Whether the log file exists at all. A board that has never written a record
// answers false, which is not an error.
bool autoStorageLogExists();

// The log opened for sequential reading, or a false File if it could not be
// opened. The caller closes it. This exists so LOGGER STORAGE DUMP can walk
// the log record by record and format each one itself without knowing the path
// or naming LittleFS.
File autoStorageOpenLogForRead();

// ----------------------------------------------------------------------------
// WRITING THE LOG
// ----------------------------------------------------------------------------

// Append one record and flush it. Returns false, having said why, on a failed
// open, a short write, or a file whose size is no longer a whole number of
// records. It does not decide that a record should be written, and it does not
// touch the sequence number the record carries.
bool autoStorageAppend(const AutoRecord &record);

// The open/write/flush+close split of the last autoStorageAppend().
AutoAppendTiming autoStorageLastAppendTiming();

// ----------------------------------------------------------------------------
// SPACE
// ----------------------------------------------------------------------------
//
// The filesystem's own numbers. What to do when the free figure reaches zero is
// an undecided policy question and is not answered here.

size_t autoStorageTotalBytes();
size_t autoStorageUsedBytes();
