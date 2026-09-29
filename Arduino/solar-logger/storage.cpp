// ============================================================================
// DURABLE STORAGE
// ============================================================================
//
// LittleFS on the stock "spiffs" partition. No partition-table change, so the
// nvs partition holding experiment state is never touched.
//
// Mount NEVER formats on failure. Formatting would destroy stored history,
// and a first-run unformatted partition is indistinguishable from a corrupted
// one at mount time. Initializing is an explicit operator action:
// LOGGER STORAGE CLEAR YES.
//
// See storage.h for the boundary: this file holds the filesystem mechanics,
// and every decision that uses them - when a record is written, which sequence
// it carries, what a clear is allowed to interrupt - stays in
// solar-logger.ino.
// ============================================================================

#include "storage.h"

// A .cpp does not get the <Arduino.h> that Arduino CLI prepends to the sketch,
// so this file includes what it uses (D-047). Arduino.h supplies Serial and
// micros(); LittleFS.h supplies the filesystem itself.
#include <Arduino.h>
#include <LittleFS.h>

// ----------------------------------------------------------------------------
// WHERE THE LOG LIVES
// ----------------------------------------------------------------------------
//
// Private to this module: nothing outside it opens the log by name. The
// temporary path exists only for the tail repair, which cannot truncate in
// place because the Arduino File API exposes no truncate.

constexpr char AUTO_LOG_PATH[] = "/auto.bin";
constexpr char AUTO_LOG_TMP_PATH[] = "/auto.tmp";

// Whether LittleFS is mounted. Private, and private is the point: the only
// ways to change it are mounting and formatting, both of which are functions
// here.
static bool autoStorageMounted = false;

// Written by autoStorageAppend() and read back through
// autoStorageLastAppendTiming(). See storage.h for why they are not exported.
static uint32_t autoAppendOpenUs = 0;
static uint32_t autoAppendWriteUs = 0;
static uint32_t autoAppendFlushCloseUs = 0;

// ----------------------------------------------------------------------------
// MOUNTING
// ----------------------------------------------------------------------------

bool autoStorageMount(bool verbose)
{
	if (autoStorageMounted)
	{
		return true;
	}

	if (!LittleFS.begin(false))
	{
		if (verbose)
		{
			Serial.println(
					"[STORAGE] ERROR: LittleFS mount failed.");
			Serial.println(
					"[STORAGE] This is expected on a partition that has never been "
					"initialized.");
			Serial.println(
					"[STORAGE] Run LOGGER STORAGE CLEAR YES to format it. Nothing is "
					"formatted automatically, because a mount failure and a corrupted "
					"filesystem look identical here.");
		}
		return false;
	}

	autoStorageMounted = true;
	return true;
}

// The format half of LOGGER STORAGE CLEAR YES. The begin() first is not
// redundant: on a partition that has never been initialized the mount fails,
// and that is the one case where formatting is the right answer and is said
// out loud before it happens.
bool autoStorageFormat()
{
	if (!LittleFS.begin(false))
	{
		Serial.println(
				"[STORAGE] Mount failed; formatting the partition to initialize it.");
	}

	if (!LittleFS.format())
	{
		Serial.println("[STORAGE] ERROR: Format failed.");
		return false;
	}

	autoStorageMounted = false;
	return true;
}

// ----------------------------------------------------------------------------
// READING THE LOG
// ----------------------------------------------------------------------------

// Full scan. At bench scale this is cheap, and one pass produces everything
// STORAGE INFO needs plus the recovery decision.
AutoLogScan autoStorageScan()
{
	AutoLogScan scan = {};
	scan.mounted = autoStorageMounted;

	if (!autoStorageMounted)
	{
		return scan;
	}

	if (!LittleFS.exists(AUTO_LOG_PATH))
	{
		scan.fileExists = false;
		return scan;
	}

	scan.fileExists = true;

	File file = LittleFS.open(AUTO_LOG_PATH, FILE_READ);

	if (!file)
	{
		Serial.println("[STORAGE] ERROR: Could not open the log for scanning.");
		return scan;
	}

	scan.fileBytes = file.size();
	scan.partialTail = (scan.fileBytes % AUTO_RECORD_SIZE) != 0;

	uint32_t previousSeq = 0;
	AutoRecord record;

	// Invalid records seen since the last valid one. At end of file this is the
	// trailing run, which is the only damage an automatic repair may remove.
	uint32_t runOfInvalid = 0;

	while (true)
	{
		size_t offset = file.position();

		if (scan.fileBytes - offset < AUTO_RECORD_SIZE)
		{
			break;
		}

		size_t got = file.read(reinterpret_cast<uint8_t *>(&record),
													 AUTO_RECORD_SIZE);

		if (got != AUTO_RECORD_SIZE)
		{
			// Not a torn tail: the file says these bytes exist and the filesystem
			// would not hand them over. What is past here is unknown, so it is
			// never automatically discarded. autoStorageRecover() refuses to
			// rewrite while this is set.
			Serial.println(
					"[STORAGE] ERROR: Short read while scanning the log. The rest of "
					"the log could not be read, so nothing will be repaired "
					"automatically.");
			scan.readError = true;
			break;
		}

		if (!autoRecordValid(record))
		{
			// Counted, and the scan KEEPS GOING. Stopping here and calling the
			// remainder "trailing" is what destroyed every good record after a
			// single bit flip; how much good data lies beyond the damage is
			// exactly the fact the caller needs in order to refuse.
			if (scan.invalidRecords == 0)
			{
				scan.firstInvalidOffset = offset;
			}

			scan.invalidRecords++;
			runOfInvalid++;
			continue;
		}

		if (scan.invalidRecords > 0)
		{
			scan.validAfterInvalidRecords++;
		}

		runOfInvalid = 0;

		if (scan.validRecords == 0)
		{
			scan.firstSeq = record.seq;
		}
		else if (record.seq <= previousSeq)
		{
			scan.seqOutOfOrder = true;
		}

		previousSeq = record.seq;
		scan.lastSeq = record.seq;
		scan.validRecords++;
		scan.validBytes += AUTO_RECORD_SIZE;
		scan.lastRunChargeUAh = record.running_charge_uAh;
		scan.lastRunEnergyUWh = record.running_energy_uWh;
	}

	file.close();

	// Only the tail is repairable, so only the tail is counted as discardable:
	// the unbroken run of invalid records at the end, plus a partial record.
	// For a log damaged only at its tail this is the same number the old
	// "everything past the first bad record" arithmetic produced.
	scan.trailingInvalidRecords = runOfInvalid;
	scan.trailingBytes =
			static_cast<size_t>(runOfInvalid) * AUTO_RECORD_SIZE +
			(scan.fileBytes % AUTO_RECORD_SIZE);
	scan.keepBytes = scan.fileBytes - scan.trailingBytes;

	return scan;
}

// Rewrite the log to exactly its valid prefix, discarding only tail damage.
//
// The Arduino File API exposes no truncate, so this copies the prefix the scan
// said to keep to a temporary file and renames it. That is O(file size), but it
// only runs when a damaged tail was actually found, which should be rare.
//
// IT REFUSES TO RUN ON DAMAGE IT CANNOT SAFELY REPAIR. This is the one function
// in the firmware that deletes stored records, so the check lives here as well
// as in its caller: a valid record after an invalid one, or a log that could
// not be read to the end, means the boundary between "damaged" and "good" is
// not known, and no amount of accurate reporting makes deleting the remainder
// recoverable afterwards.
//
// IT IS DELIBERATELY NOT `static`, although autoStorageScanAndRepair() below is
// its only caller. Measured on 2026-09-24: with internal linkage GCC inlined it
// into that one call site, proved from the enclosing `if`/`else if` chain that
// `validAfterInvalidRecords > 0 || scan.readError` could not hold there, and
// deleted the refusal and its message from the image. The check would come back
// the moment a second caller existed, but until then the firmware would not
// contain a refusal the source says it contains, which is the kind of gap
// between belief and behavior this project does not accept. External linkage
// keeps it compiled, at 614 bytes.
bool autoStorageTruncateToValid(const AutoLogScan &scan)
{
	if (scan.validAfterInvalidRecords > 0 || scan.readError)
	{
		Serial.println(
				"[STORAGE] ERROR: Refusing to rewrite the log. The damage is not "
				"confined to the tail, so a rewrite would discard good records.");
		return false;
	}

	Serial.print("[STORAGE] Rewriting the log to its valid prefix: ");
	Serial.print(static_cast<unsigned long>(scan.keepBytes));
	Serial.print(" of ");
	Serial.print(static_cast<unsigned long>(scan.fileBytes));
	Serial.println(" bytes.");

	File source = LittleFS.open(AUTO_LOG_PATH, FILE_READ);

	if (!source)
	{
		Serial.println("[STORAGE] ERROR: Could not open the log for recovery.");
		return false;
	}

	File destination = LittleFS.open(AUTO_LOG_TMP_PATH, FILE_WRITE);

	if (!destination)
	{
		source.close();
		Serial.println(
				"[STORAGE] ERROR: Could not create the recovery temporary file.");
		return false;
	}

	uint8_t buffer[AUTO_RECORD_SIZE];
	size_t copied = 0;
	bool ok = true;

	while (copied < scan.keepBytes)
	{
		size_t got = source.read(buffer, AUTO_RECORD_SIZE);

		if (got != AUTO_RECORD_SIZE)
		{
			Serial.println("[STORAGE] ERROR: Short read during recovery copy.");
			ok = false;
			break;
		}

		if (destination.write(buffer, AUTO_RECORD_SIZE) != AUTO_RECORD_SIZE)
		{
			Serial.println("[STORAGE] ERROR: Short write during recovery copy.");
			ok = false;
			break;
		}

		copied += AUTO_RECORD_SIZE;
	}

	destination.flush();
	destination.close();
	source.close();

	if (!ok)
	{
		LittleFS.remove(AUTO_LOG_TMP_PATH);
		Serial.println(
				"[STORAGE] ERROR: Recovery aborted. The original log is unchanged.");
		return false;
	}

	if (!LittleFS.remove(AUTO_LOG_PATH))
	{
		LittleFS.remove(AUTO_LOG_TMP_PATH);
		Serial.println("[STORAGE] ERROR: Could not remove the damaged log.");
		return false;
	}

	if (!LittleFS.rename(AUTO_LOG_TMP_PATH, AUTO_LOG_PATH))
	{
		Serial.println(
				"[STORAGE] ERROR: Could not rename the recovered log into place. "
				"The recovered data is still at the temporary path.");
		return false;
	}

	Serial.println("[STORAGE] Log recovered to its valid prefix: ALL OK");
	return true;
}

// Boot recovery of the FILE. Returns what the scan found, so the caller can
// decide the next sequence from it.
//
// Nothing here is silent: every discarded byte is counted and reported, and
// every case that was NOT repaired says so.
//
// The sequence decision that used to sit at the end of this function is not
// gone; it is in autoStorageRecover() in solar-logger.ino, which owns the NVS
// reservation floor and the RTC-retained totals (D-055).
AutoLogScan autoStorageScanAndRepair()
{
	AutoLogScan scan = autoStorageScan();

	Serial.println("[STORAGE] Scanning durable log...");

	if (!scan.fileExists)
	{
		Serial.println("[STORAGE] No log file yet; it will be created.");
	}
	else
	{
		Serial.print("[STORAGE] File bytes:     ");
		Serial.println(static_cast<unsigned long>(scan.fileBytes));

		Serial.print("[STORAGE] Valid records:  ");
		Serial.println(scan.validRecords);

		if (scan.validRecords > 0)
		{
			Serial.print("[STORAGE] Sequence range: ");
			Serial.print(scan.firstSeq);
			Serial.print(" -> ");
			Serial.println(scan.lastSeq);
		}
	}

	if (scan.seqOutOfOrder)
	{
		Serial.println(
				"[STORAGE] WARNING: Sequence numbers are not strictly increasing. "
				"The log may contain records from an earlier reuse.");
	}

	if (scan.partialTail)
	{
		Serial.print(
				"[STORAGE] WARNING: File size is not a whole number of records. "
				"Remainder: ");
		Serial.print(static_cast<unsigned long>(scan.fileBytes % AUTO_RECORD_SIZE));
		Serial.println(" bytes. This indicates a power loss during a write.");
	}

	if (scan.invalidRecords > 0)
	{
		Serial.print("[STORAGE] WARNING: ");
		Serial.print(scan.invalidRecords);
		Serial.print(" record(s) failed validation, the first at offset ");
		Serial.print(static_cast<unsigned long>(scan.firstInvalidOffset));
		Serial.println(". Magic, version, or CRC did not match.");
	}

	// Which damage case this is, and therefore what may be done
	// about it without an operator. Said out loud in every case, because a
	// repair that ran and a repair that was refused must never look alike.
	if (scan.readError)
	{
		Serial.println(
				"[STORAGE] The log could not be read to the end, so what lies past "
				"the failed read is unknown. NOTHING WAS DISCARDED. Read the log with "
				"LOGGER STORAGE DUMP before deciding.");
	}
	else if (scan.validAfterInvalidRecords > 0)
	{
		// The defect this branch exists to prevent: the old code called every
		// byte past the first bad record "trailing" and deleted it at boot,
		// destroying good data before anyone saw the message.
		Serial.print("[STORAGE] WARNING: ");
		Serial.print(scan.validAfterInvalidRecords);
		Serial.println(
				" valid record(s) lie AFTER an invalid one. This is mid-file damage, "
				"not a torn tail.");
		Serial.println(
				"[STORAGE] NOTHING WAS DISCARDED. Repairing this automatically would "
				"delete those good records, so it is an operator decision. Read the "
				"log with LOGGER STORAGE DUMP, which reports every record including "
				"the invalid ones.");
		Serial.println(
				"[STORAGE] Appending continues normally, and the NVS reservation "
				"floor applies because the tail is not provably intact.");
	}
	else if (scan.trailingBytes > 0)
	{
		Serial.print("[STORAGE] Discarding ");
		Serial.print(static_cast<unsigned long>(scan.trailingBytes));
		Serial.print(" trailing bytes that could not be validated (");
		Serial.print(scan.trailingInvalidRecords);
		Serial.print(" whole record(s) plus ");
		Serial.print(static_cast<unsigned long>(scan.fileBytes % AUTO_RECORD_SIZE));
		Serial.println(" partial byte(s)).");

		if (!autoStorageTruncateToValid(scan))
		{
			Serial.println(
					"[STORAGE] ERROR: Could not repair the log tail. Appending is "
					"unsafe until this is resolved.");
		}
	}
	else if (scan.fileExists)
	{
		Serial.println("[STORAGE] Log tail is intact: ALL OK");
	}

	return scan;
}

// The two primitives LOGGER STORAGE DUMP needs, so that command can walk the
// log and format each record itself without knowing the path.
//
// They repeat the open that autoStorageScan() performs inline rather than
// replacing it. The scan's body is deployed, characterized behavior and this
// extraction deliberately left every moved body identical; folding the two
// together is a change to make on purpose, with its own test run, not on the
// way past.

bool autoStorageLogExists()
{
	return LittleFS.exists(AUTO_LOG_PATH);
}

File autoStorageOpenLogForRead()
{
	return LittleFS.open(AUTO_LOG_PATH, FILE_READ);
}

// ----------------------------------------------------------------------------
// WRITING THE LOG
// ----------------------------------------------------------------------------

bool autoStorageAppend(const AutoRecord &record)
{
	const uint32_t tOpenStart = micros();

	File file = LittleFS.open(AUTO_LOG_PATH, FILE_APPEND, true);

	const uint32_t tOpened = micros();
	autoAppendOpenUs = tOpened - tOpenStart;

	if (!file)
	{
		autoAppendWriteUs = 0;
		autoAppendFlushCloseUs = 0;
		Serial.println("[STORAGE] ERROR: Could not open the log for append.");
		return false;
	}

	size_t written = file.write(reinterpret_cast<const uint8_t *>(&record),
															AUTO_RECORD_SIZE);

	const uint32_t tWritten = micros();
	autoAppendWriteUs = tWritten - tOpened;

	file.flush();
	size_t sizeAfter = file.size();
	file.close();

	autoAppendFlushCloseUs = micros() - tWritten;

	if (written != AUTO_RECORD_SIZE)
	{
		Serial.print("[STORAGE] ERROR: Short write. Wrote ");
		Serial.print(static_cast<unsigned long>(written));
		Serial.print(" of ");
		Serial.print(static_cast<unsigned long>(AUTO_RECORD_SIZE));
		Serial.println(" bytes. The tail is now partial.");
		return false;
	}

	if (sizeAfter % AUTO_RECORD_SIZE != 0)
	{
		Serial.println(
				"[STORAGE] ERROR: File size is not a whole number of records after "
				"append.");
		return false;
	}

	return true;
}

AutoAppendTiming autoStorageLastAppendTiming()
{
	return AutoAppendTiming{autoAppendOpenUs, autoAppendWriteUs,
													autoAppendFlushCloseUs};
}

// ----------------------------------------------------------------------------
// SPACE
// ----------------------------------------------------------------------------

size_t autoStorageTotalBytes()
{
	return LittleFS.totalBytes();
}

size_t autoStorageUsedBytes()
{
	return LittleFS.usedBytes();
}
