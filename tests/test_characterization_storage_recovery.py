"""Durable-log scan and boot recovery, executed on the host.

WHAT RUNS HERE
==============

The firmware's own storage functions are taken out of the sketch through
tests/firmware_source.py, compiled against a LittleFS made of real files in a
pytest temporary directory, and run:

  autoStorageMount()             mount, which never formats
  autoStorageScan()              one pass over the log
  autoStorageTruncateToValid()   rewrite the log to what the scan kept
  autoStorageRecover()           boot recovery, and the next sequence
  printStorageInfo()             the operator-visible LOGGER STORAGE INFO

`record_format.cpp` is linked in unmodified, so `autoRecordValid()` is the real
validator and every fixture below is a record by the deployed contract rather
than by this file's opinion of one. The one substitution is
`esp_rom_crc32_le()`, which lives in the ESP32 mask ROM and cannot be linked on
the host. It is the same shim tests/test_characterization_record.py uses, and
that file is what makes it evidence: it asserts the shim reproduces
0xFB25DA73, the CRC the board itself stored for Experiment 3 record seq=4445.

WHY THIS FILE EXISTS
====================

Four kinds of damage look alike from a distance and are not the same problem:

  TORN TAIL       whole records followed by 1..71 leftover bytes. The ordinary
                  power-loss case: a write that did not finish.
  INVALID TAIL    a complete 72-byte record at the end that fails magic,
                  version or CRC. Also a write that did not land, but framed.
  MID-FILE        a bad record with good records after it. Not a torn write at
                  all: a bit flip, or a bad block, in already-settled data.
  UNREADABLE      a read that comes up short before end of file. An I/O fault,
                  and the only one of the four where what is damaged is not
                  even known.

Until 2026-09-24 `autoStorageScan()` stopped at the first record that failed
validation and called every byte past it "trailing", and `autoStorageRecover()`
rewrote the log to that prefix automatically at boot. For the first two cases
that is right, which is why the defect never showed on hardware. For the third
it deleted every good record after the damage. Measured on this harness against
the pre-fix code, a four-record log damaged in slot 1 went in at 288 bytes and
came out at 72: sequences 12 and 13 were destroyed, at boot, before an operator
could read the message saying so.

THE POLICY THESE TESTS PIN
==========================

From docs/BACKLOG.md ("One corrupt record discards every valid record after
it") and docs/STORAGE_SYNC_DESIGN.md Section 11:

  - scan the whole file, so how much good data lies past the damage is known
  - automatically discard only a genuinely TRAILING run of bad records, plus a
    partial-record remainder
  - if any valid record survives after an invalid one, report it and REFUSE to
    rewrite; that is an operator decision, not a boot-path one
  - if the log cannot be read to the end, repair nothing at all
  - never hand a corrupt record to a host as if it were good
  - count and print everything discarded, and never let a refused repair and a
    performed repair read alike

WHAT IS NOT COVERED
===================

A log whose damage is not a whole number of records - eight junk bytes spliced
into the middle - throws every later record out of frame, so nothing after it
validates and no valid suffix is detectable. Recovering it needs an explicit
resynchronization policy, which this project does not have. The behavior is
pinned below as what it is, not as what it should be.

The board's live Experiment 3 log is never involved. Every fixture here is
synthetic and lives in a pytest temporary directory.
"""

from __future__ import annotations

import shutil
import struct
import subprocess
import zlib
from dataclasses import dataclass
from pathlib import Path

import pytest

import firmware_source

from conftest import PROJECT_ROOT

RECORD_FORMAT_HEADER = PROJECT_ROOT / "Arduino" / "solar-logger" / "record_format.h"
RECORD_FORMAT_SOURCE = PROJECT_ROOT / "Arduino" / "solar-logger" / "record_format.cpp"

RECORD_SIZE = 72

# The deployed layout, as record_format.h freezes it. Written out here rather
# than imported so this file states the bytes it lays down;
# tests/test_characterization_record.py is what proves the layout is the board's.
RECORD_STRUCT = "<HBBIqqIIIIIIiiiii"

AUTO_LOG_NAME = "auto.bin"
AUTO_LOG_TMP_NAME = "auto.tmp"


# ============================================================================
# Fixtures: bytes that are records by the deployed contract
# ============================================================================


def record_bytes(seq: int, *, charge: int = 0, energy: int = 0) -> bytes:
    """One valid 72-byte record, sealed the way the firmware seals one.

    Most values are deliberately boring; what a scan reads is the magic, the
    version, the sequence and the CRC. `charge` and `energy` are the running
    totals `autoStorageRecover()` reseeds RTC state from, so they are settable
    to show WHICH record a reseed came from.
    """

    covered = struct.pack(
        RECORD_STRUCT,
        0xB5A5,  # magic
        1,  # version
        0x00,  # flags
        seq,
        charge,  # running_charge_uAh
        energy,  # running_energy_uWh
        3,  # experiment_id
        26,  # boot_id
        60_000,  # session_elapsed_ms
        0,  # epoch_s
        60_000,  # interval_ms
        13_051_367,  # bus_uV
        -616,  # avg_current_uA
        8025,  # avg_power_uW
        18_695,  # temp_mC
        -10,  # interval_charge_uAh
        129,  # interval_energy_uWh
    )

    assert len(covered) == RECORD_SIZE - 4, "the CRC covers bytes 0..67"

    return covered + struct.pack("<I", zlib.crc32(covered) & 0xFFFFFFFF)


def reseal(damaged: bytearray) -> bytes:
    """Recompute the CRC over damaged bytes, so it is not the reason they fail.

    Corrupting the magic or the version normally breaks the CRC too, which would
    let a firmware that had dropped those checks pass anyway.
    """

    struct.pack_into("<I", damaged, 68, zlib.crc32(bytes(damaged[:68])) & 0xFFFFFFFF)
    return bytes(damaged)


def with_broken_crc(record: bytes) -> bytes:
    """A complete, correctly framed record whose CRC no longer matches.

    One flipped bit inside the covered range, which is what a flash bit flip in
    settled data looks like: right size, right magic, right version, wrong seal.
    """

    damaged = bytearray(record)
    damaged[40] ^= 0x01  # interval_ms, inside the covered range
    return bytes(damaged)


def with_broken_magic(record: bytes) -> bytes:
    """72 structurally fine bytes that are not one of our records."""

    damaged = bytearray(record)
    struct.pack_into("<H", damaged, 0, 0x1234)
    return reseal(damaged)


def with_broken_version(record: bytes) -> bytes:
    """A record version this firmware does not know how to read."""

    damaged = bytearray(record)
    damaged[2] = 2
    return reseal(damaged)


def sequences(blob: bytes) -> list[int]:
    """The `seq` field of every 72-byte slot, valid or not."""

    whole = len(blob) - (len(blob) % RECORD_SIZE)
    return [
        struct.unpack_from("<I", blob, at + 4)[0] for at in range(0, whole, RECORD_SIZE)
    ]


# ============================================================================
# The harness: the firmware's own storage functions, compiled for the host
# ============================================================================

# Identical to the shim in tests/test_characterization_record.py, and evidence
# for the same reason: that file asserts it reproduces the CRC the board stored.
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

# A LittleFS made of real files. The firmware's paths are absolute ("/auto.bin"),
# so they are rebased onto a directory this process is told about; nothing the
# firmware code opens can reach outside it.
HARNESS_PRELUDE = r"""
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "record_format.h"

// ---------------------------------------------------------------------------
// Serial, recorded rather than printed, so a test can read what an operator
// would have read.
// ---------------------------------------------------------------------------

struct RecordingSerial
{
	std::string text;

	void print(const char *value) { text += value; }
	void print(unsigned int value) { text += std::to_string(value); }
	void print(unsigned long value) { text += std::to_string(value); }

	void println() { text += '\n'; }
	void println(const char *value) { text += value; text += '\n'; }
	void println(unsigned int value) { text += std::to_string(value); text += '\n'; }
	void println(unsigned long value) { text += std::to_string(value); text += '\n'; }
};

static RecordingSerial Serial;

// ---------------------------------------------------------------------------
// LittleFS and File, backed by the filesystem.
// ---------------------------------------------------------------------------

constexpr const char *FILE_READ = "rb";
constexpr const char *FILE_WRITE = "wb";

static std::filesystem::path g_root;

// Bytes added to the size every opened File reports, without adding any bytes to
// the file. A real short read is a filesystem fault and cannot be staged with an
// ordinary file, so this stages one: the firmware is told the log is longer than
// it is, and its read of the last record comes up short. Zero in every test but
// the one that exercises that path.
static size_t g_phantomBytes = 0;

static std::filesystem::path rebased(const char *path)
{
	const char *relative = (path[0] == '/') ? path + 1 : path;
	return g_root / relative;
}

class HostFile
{
public:
	HostFile() = default;
	HostFile(std::FILE *handle, size_t bytes) : handle_(handle), bytes_(bytes) {}

	explicit operator bool() const { return handle_ != nullptr; }

	size_t size() const { return bytes_; }

	size_t position()
	{
		if (handle_ == nullptr) { return 0; }

		long where = std::ftell(handle_);
		return (where < 0) ? 0 : static_cast<size_t>(where);
	}

	size_t read(uint8_t *into, size_t count)
	{
		return (handle_ == nullptr) ? 0 : std::fread(into, 1, count, handle_);
	}

	size_t write(const uint8_t *from, size_t count)
	{
		return (handle_ == nullptr) ? 0 : std::fwrite(from, 1, count, handle_);
	}

	void flush() { if (handle_ != nullptr) { std::fflush(handle_); } }

	void close()
	{
		if (handle_ != nullptr) { std::fclose(handle_); handle_ = nullptr; }
	}

private:
	std::FILE *handle_ = nullptr;
	size_t bytes_ = 0;
};

using File = HostFile;

class HostLittleFS
{
public:
	// The firmware calls begin(false): mount, never format. A directory that
	// exists is a mounted filesystem here; a missing one is a failed mount.
	bool begin(bool formatOnFail)
	{
		(void)formatOnFail;
		return std::filesystem::exists(g_root);
	}

	bool exists(const char *path) { return std::filesystem::exists(rebased(path)); }

	File open(const char *path, const char *mode)
	{
		std::filesystem::path full = rebased(path);
		std::FILE *handle = std::fopen(full.c_str(), mode);

		if (handle == nullptr) { return File(); }

		std::error_code ignored;
		uintmax_t bytes = std::filesystem::file_size(full, ignored);

		return File(handle,
								ignored ? 0 : static_cast<size_t>(bytes) + g_phantomBytes);
	}

	bool remove(const char *path)
	{
		std::error_code error;
		return std::filesystem::remove(rebased(path), error) && !error;
	}

	bool rename(const char *from, const char *to)
	{
		std::error_code error;
		std::filesystem::rename(rebased(from), rebased(to), error);
		return !error;
	}

	// Fixed, so printStorageInfo()'s capacity projection is arithmetic on known
	// numbers rather than a property of whatever disk the tests run on.
	size_t totalBytes() const { return 1441792; }

	size_t usedBytes() const
	{
		std::error_code error;
		uintmax_t bytes = std::filesystem::file_size(rebased("/auto.bin"), error);
		return error ? 0 : static_cast<size_t>(bytes);
	}
};

static HostLittleFS LittleFS;

// ---------------------------------------------------------------------------
// The globals the extracted functions read and write.
// ---------------------------------------------------------------------------

constexpr char AUTO_LOG_PATH[] = "/auto.bin";
constexpr char AUTO_LOG_TMP_PATH[] = "/auto.tmp";

static bool autoStorageMounted = false;
static uint32_t autonomousIntervalSeconds = 60;

static uint32_t rtcAutoSeqHighWater = 0;
static int64_t rtcAutoRunChargeUAh = 0;
static int64_t rtcAutoRunEnergyUWh = 0;

// The NVS reservation floor, supplied on the command line so a test can place
// it above or below the log's own tail.
static uint32_t g_highWater = 0;

uint32_t loadSequenceHighWater() { return g_highWater; }
"""

HARNESS_DRIVER = r"""
static void reportScan(const AutoLogScan &scan)
{
	std::printf("mounted=%d\n", scan.mounted ? 1 : 0);
	std::printf("fileExists=%d\n", scan.fileExists ? 1 : 0);
	std::printf("validRecords=%u\n", (unsigned)scan.validRecords);
	std::printf("firstSeq=%u\n", (unsigned)scan.firstSeq);
	std::printf("lastSeq=%u\n", (unsigned)scan.lastSeq);
	std::printf("fileBytes=%llu\n", (unsigned long long)scan.fileBytes);
	std::printf("validBytes=%llu\n", (unsigned long long)scan.validBytes);
	std::printf("trailingBytes=%llu\n", (unsigned long long)scan.trailingBytes);
	std::printf("keepBytes=%llu\n", (unsigned long long)scan.keepBytes);
	std::printf("invalidRecords=%u\n", (unsigned)scan.invalidRecords);
	std::printf("trailingInvalidRecords=%u\n",
							(unsigned)scan.trailingInvalidRecords);
	std::printf("validAfterInvalidRecords=%u\n",
							(unsigned)scan.validAfterInvalidRecords);
	std::printf("firstInvalidOffset=%llu\n",
							(unsigned long long)scan.firstInvalidOffset);
	std::printf("partialTail=%d\n", scan.partialTail ? 1 : 0);
	std::printf("readError=%d\n", scan.readError ? 1 : 0);
	std::printf("seqOutOfOrder=%d\n", scan.seqOutOfOrder ? 1 : 0);
	std::printf("lastRunChargeUAh=%lld\n", (long long)scan.lastRunChargeUAh);
	std::printf("lastRunEnergyUWh=%lld\n", (long long)scan.lastRunEnergyUWh);
}

int main(int argc, char **argv)
{
	if (argc < 3)
	{
		std::fprintf(stderr,
								 "usage: %s scan|recover|info|force-truncate <root> [highWater]\n",
								 argv[0]);
		return 2;
	}

	const char *command = argv[1];
	g_root = std::filesystem::path(argv[2]);
	g_highWater = (argc > 3) ? (uint32_t)std::strtoul(argv[3], nullptr, 10) : 0;
	g_phantomBytes = (argc > 4) ? (size_t)std::strtoul(argv[4], nullptr, 10) : 0;

	if (!autoStorageMount(true))
	{
		std::printf("mountFailed=1\n");
		std::printf("--- serial ---\n%s", Serial.text.c_str());
		return 0;
	}

	std::printf("mountFailed=0\n");

	if (std::strcmp(command, "scan") == 0)
	{
		reportScan(autoStorageScan());
		std::printf("--- serial ---\n%s", Serial.text.c_str());
		return 0;
	}

	if (std::strcmp(command, "recover") == 0)
	{
		uint32_t next = autoStorageRecover();
		std::printf("nextSeq=%u\n", (unsigned)next);
		std::printf("rtcRunCharge=%lld\n", (long long)rtcAutoRunChargeUAh);
		std::printf("rtcRunEnergy=%lld\n", (long long)rtcAutoRunEnergyUWh);
		std::printf("--- serial ---\n%s", Serial.text.c_str());
		return 0;
	}

	if (std::strcmp(command, "info") == 0)
	{
		std::printf("infoOk=%d\n", printStorageInfo() ? 1 : 0);
		std::printf("--- serial ---\n%s", Serial.text.c_str());
		return 0;
	}

	// Calls the destroying function DIRECTLY, bypassing the caller that decides
	// whether it should run, so its own refusal can be tested rather than
	// assumed to be unreachable.
	if (std::strcmp(command, "force-truncate") == 0)
	{
		AutoLogScan scan = autoStorageScan();
		std::printf("truncated=%d\n", autoStorageTruncateToValid(scan) ? 1 : 0);
		std::printf("--- serial ---\n%s", Serial.text.c_str());
		return 0;
	}

	std::fprintf(stderr, "unknown command: %s\n", command);
	return 2;
}
"""

# The sketch functions the harness compiles, with the return type each must
# still have. A changed signature fails by name here rather than being compiled
# into something these tests did not mean.
EXTRACTED = (
    ("autoStorageMount", "bool"),
    ("autoStorageScan", "AutoLogScan"),
    ("autoStorageTruncateToValid", "bool"),
    ("autoStorageRecover", "uint32_t"),
    ("printStorageInfo", "bool"),
)


@dataclass(frozen=True)
class Result:
    """One harness call: its reported numbers, and what the operator was told."""

    fields: dict[str, int]
    serial: str

    def __getitem__(self, name: str) -> int:
        return self.fields[name]


class Storage:
    """The firmware's storage functions, compiled and run against real files."""

    def __init__(self, binary: Path, root: Path):
        self.binary = binary
        self.root = root

    # -- the log ------------------------------------------------------------

    def write_log(self, *chunks: bytes) -> None:
        """Lay down a log file made of exactly these bytes, in order."""

        (self.root / AUTO_LOG_NAME).write_bytes(b"".join(chunks))

    def log_bytes(self) -> bytes:
        path = self.root / AUTO_LOG_NAME
        return path.read_bytes() if path.exists() else b""

    def temp_exists(self) -> bool:
        """Whether a recovery temporary file was left behind."""

        return (self.root / AUTO_LOG_TMP_NAME).exists()

    # -- driving ------------------------------------------------------------

    def _run(self, command: str, *arguments: object) -> Result:
        result = subprocess.run(
            [
                str(self.binary),
                command,
                str(self.root),
                *(str(argument) for argument in arguments),
            ],
            capture_output=True,
            text=True,
            check=False,
        )

        if result.returncode != 0:
            raise RuntimeError(
                f"harness `{command}` exited {result.returncode}: {result.stderr}"
            )

        head, _, serial = result.stdout.partition("--- serial ---\n")
        fields = {
            key: int(value)
            for key, _, value in (line.partition("=") for line in head.splitlines())
            if key
        }

        return Result(fields, serial)

    def scan(self, phantom_bytes: int = 0) -> Result:
        return self._run("scan", 0, phantom_bytes)

    def recover(self, high_water: int = 0, phantom_bytes: int = 0) -> Result:
        return self._run("recover", high_water, phantom_bytes)

    def info(self) -> Result:
        return self._run("info")

    def force_truncate(self) -> Result:
        return self._run("force-truncate")


@pytest.fixture(scope="module")
def harness(tmp_path_factory: pytest.TempPathFactory) -> Path:
    """Compile the firmware's storage functions once for the whole module."""

    compiler = shutil.which("c++")

    if compiler is None:
        pytest.fail(
            "No host C++ compiler (`c++`) on PATH. These tests compile the "
            "firmware's own scan and recovery functions and run them against "
            "damaged logs; without a compiler the recovery policy has no "
            "executed cover, so this is a failure, not a skip."
        )

    build = tmp_path_factory.mktemp("storage_recovery")
    (build / "esp_rom_crc.h").write_text(CRC_SHIM_HEADER, encoding="utf-8")
    (build / "crc_shim.cpp").write_text(CRC_SHIM_SOURCE, encoding="utf-8")

    sketch = firmware_source.load()
    source = "\n\n".join(
        [
            HARNESS_PRELUDE,
            str(sketch.type_definition("struct", "AutoLogScan")),
            *(str(sketch.definition(name, returns)) for name, returns in EXTRACTED),
            HARNESS_DRIVER,
        ]
    )
    (build / "harness.cpp").write_text(source, encoding="utf-8")

    binary = build / "storage_harness"

    # -std=c++20 because the ESP32 core builds with -std=gnu++2a, and -Werror so
    # a sign-compare or a narrowing conversion in the recovery arithmetic fails
    # here rather than on the board.
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
            "The firmware's storage functions did not compile on the host:\n"
            f"{result.stderr}\n--- harness source ---\n{source}"
        )

    return binary


@pytest.fixture
def storage(harness: Path, tmp_path: Path) -> Storage:
    """A fresh, empty filesystem per test."""

    root = tmp_path / "littlefs"
    root.mkdir()
    return Storage(harness, root)


# ============================================================================
# The harness itself, before anything is concluded from it
# ============================================================================


def test_the_fixtures_are_records_by_the_deployed_contract(storage: Storage):
    """Every `valid` fixture below is valid according to autoRecordValid()."""

    storage.write_log(record_bytes(1), record_bytes(2), record_bytes(3))
    scan = storage.scan()

    assert scan["validRecords"] == 3
    assert scan["invalidRecords"] == 0


@pytest.mark.parametrize(
    "damage",
    (with_broken_crc, with_broken_magic, with_broken_version),
    ids=("crc", "magic", "version"),
)
def test_every_damaged_fixture_really_does_fail_validation(storage: Storage, damage):
    """Otherwise a test that "proves" corruption is preserved proves nothing.

    The magic and version fixtures are resealed, so a correct CRC is not what
    saves them: each fails for its own reason.
    """

    storage.write_log(damage(record_bytes(1)))
    scan = storage.scan()

    assert scan["validRecords"] == 0
    assert scan["invalidRecords"] == 1


def test_a_missing_filesystem_is_a_failed_mount_and_not_a_format(
    harness: Path, tmp_path: Path
):
    """Mount never formats: D-023 and STORAGE_SYNC_DESIGN Section 11."""

    absent = Storage(harness, tmp_path / "never-created")
    result = absent._run("scan")

    assert result["mountFailed"] == 1
    assert "mount failed" in result.serial
    assert "Nothing is formatted automatically" in result.serial
    assert not (tmp_path / "never-created").exists(), "mount created a filesystem"


# ============================================================================
# A. A completely valid log is left alone
# ============================================================================


def test_a_valid_log_survives_recovery_byte_for_byte(storage: Storage):
    before = b"".join(record_bytes(seq) for seq in (10, 11, 12))
    storage.write_log(before)

    result = storage.recover()

    assert storage.log_bytes() == before
    assert result["nextSeq"] == 13
    assert "Log tail is intact: ALL OK" in result.serial
    assert not storage.temp_exists()


def test_a_valid_log_is_the_sequence_authority_over_the_nvs_floor(storage: Storage):
    """D-023, unchanged by this work: an intact tail outranks the reservation."""

    storage.write_log(*(record_bytes(seq) for seq in (10, 11, 12)))

    result = storage.recover(high_water=5000)

    assert result["nextSeq"] == 13
    assert "provably intact" in result.serial


def test_a_valid_log_reseeds_the_running_totals_from_its_last_record(
    storage: Storage,
):
    storage.write_log(
        record_bytes(10, charge=100, energy=200),
        record_bytes(11, charge=300, energy=400),
    )

    result = storage.recover()

    assert result["rtcRunCharge"] == 300
    assert result["rtcRunEnergy"] == 400


# ============================================================================
# B. A torn tail: whole records, then leftover bytes
# ============================================================================


def test_a_partial_tail_is_discarded_and_counted(storage: Storage):
    """STORAGE_SYNC_DESIGN Section 11 step 2. The ordinary power-loss case."""

    storage.write_log(record_bytes(10), record_bytes(11), b"\x00" * 40)

    result = storage.recover()

    assert len(storage.log_bytes()) == 2 * RECORD_SIZE
    assert sequences(storage.log_bytes()) == [10, 11]
    assert result["nextSeq"] == 12
    assert "Remainder: 40 bytes" in result.serial
    assert "Discarding 40 trailing bytes" in result.serial
    assert not storage.temp_exists()


def test_a_partial_tail_is_not_treated_as_an_invalid_record(storage: Storage):
    """40 leftover bytes are not a record that failed: they are not a record."""

    storage.write_log(record_bytes(10), b"\x00" * 40)
    scan = storage.scan()

    assert scan["partialTail"] == 1
    assert scan["invalidRecords"] == 0
    assert scan["trailingInvalidRecords"] == 0
    assert scan["trailingBytes"] == 40


def test_a_file_shorter_than_one_record_is_discarded_whole(storage: Storage):
    storage.write_log(b"\x00" * 40)

    result = storage.recover()

    assert storage.log_bytes() == b""
    assert result["nextSeq"] == 1
    assert "Discarding 40 trailing bytes" in result.serial


# ============================================================================
# C. An invalid tail: a complete record at the end that fails validation
# ============================================================================


@pytest.mark.parametrize(
    "damage",
    (with_broken_crc, with_broken_magic, with_broken_version),
    ids=("crc", "magic", "version"),
)
def test_an_invalid_final_record_is_discarded(storage: Storage, damage):
    """Section 11 step 4, for the case where the backward walk stops at once."""

    storage.write_log(record_bytes(10), record_bytes(11), damage(record_bytes(12)))

    result = storage.recover()

    assert sequences(storage.log_bytes()) == [10, 11]
    assert result["nextSeq"] == 12
    assert "Discarding 72 trailing bytes" in result.serial
    assert not storage.temp_exists()


def test_a_trailing_run_of_invalid_records_is_discarded_together(storage: Storage):
    """Two bad records at the end are still only tail damage."""

    storage.write_log(
        record_bytes(10),
        with_broken_crc(record_bytes(11)),
        with_broken_crc(record_bytes(12)),
    )

    result = storage.recover()

    assert sequences(storage.log_bytes()) == [10]
    assert "Discarding 144 trailing bytes" in result.serial
    assert "2 whole record(s) plus 0 partial byte(s)" in result.serial


def test_an_invalid_tail_and_a_partial_remainder_are_discarded_together(
    storage: Storage,
):
    storage.write_log(
        record_bytes(10), with_broken_crc(record_bytes(11)), b"\xff" * 30
    )

    result = storage.recover()

    assert sequences(storage.log_bytes()) == [10]
    assert "Discarding 102 trailing bytes" in result.serial
    assert "1 whole record(s) plus 30 partial byte(s)" in result.serial


def test_a_log_of_nothing_but_one_bad_record_is_emptied(storage: Storage):
    """No valid record is stranded, so this is still only tail damage."""

    storage.write_log(with_broken_crc(record_bytes(10)))

    result = storage.recover()

    assert storage.log_bytes() == b""
    assert result["nextSeq"] == 1
    assert "no auto" not in result.serial.lower()


# ============================================================================
# D. Mid-file damage: the defect
# ============================================================================
#
# Against the pre-fix firmware every test in this section fails. The scan
# stopped at slot 1, `trailingBytes` became every byte from there to the end,
# and `autoStorageRecover()` rewrote the log to 72 bytes at boot.


def test_a_valid_record_after_an_invalid_one_is_not_discarded(storage: Storage):
    """THE DEFECT. Four slots in, four slots out, nothing deleted."""

    before = b"".join(
        (
            record_bytes(10),
            with_broken_crc(record_bytes(11)),
            record_bytes(12),
            record_bytes(13),
        )
    )
    storage.write_log(before)

    storage.recover()

    assert storage.log_bytes() == before, "recovery rewrote a mid-file-damaged log"
    assert sequences(storage.log_bytes()) == [10, 11, 12, 13]
    assert not storage.temp_exists()


def test_mid_file_damage_is_reported_as_what_it_is(storage: Storage):
    """A refused repair and a performed repair must never read alike."""

    storage.write_log(
        record_bytes(10),
        with_broken_crc(record_bytes(11)),
        record_bytes(12),
        record_bytes(13),
    )

    result = storage.recover()

    assert "2 valid record(s) lie AFTER an invalid one" in result.serial
    assert "mid-file damage, not a torn tail" in result.serial
    assert "NOTHING WAS DISCARDED" in result.serial
    assert "Discarding" not in result.serial
    assert "Log tail is intact: ALL OK" not in result.serial
    assert "Log recovered to its valid prefix" not in result.serial


def test_the_scan_counts_the_good_data_stranded_past_the_damage(storage: Storage):
    """How much is at stake is the fact the refusal is made on."""

    storage.write_log(
        record_bytes(10),
        with_broken_crc(record_bytes(11)),
        record_bytes(12),
        record_bytes(13),
    )

    scan = storage.scan()

    assert scan["validRecords"] == 3
    assert scan["invalidRecords"] == 1
    assert scan["validAfterInvalidRecords"] == 2
    assert scan["trailingInvalidRecords"] == 0
    assert scan["firstInvalidOffset"] == 72
    assert scan["trailingBytes"] == 0, "nothing at the tail is discardable"
    assert scan["keepBytes"] == 4 * RECORD_SIZE
    assert scan["firstSeq"] == 10
    assert scan["lastSeq"] == 13


def test_damage_in_the_very_first_record_strands_everything_after_it(
    storage: Storage,
):
    before = b"".join((with_broken_crc(record_bytes(10)), record_bytes(11)))
    storage.write_log(before)

    result = storage.recover()

    assert storage.log_bytes() == before
    assert result["nextSeq"] == 12
    assert "NOTHING WAS DISCARDED" in result.serial


def test_the_next_sequence_comes_from_the_last_valid_record_in_the_whole_file(
    storage: Storage,
):
    """Not from the last one before the damage.

    Pre-fix this returned 11, one past the record before the corruption, while
    records 12 and 13 were being deleted. Preserving them and then handing out
    sequence 11 again would be the duplicate D-018 forbids.
    """

    storage.write_log(
        record_bytes(10),
        with_broken_crc(record_bytes(11)),
        record_bytes(12),
        record_bytes(13),
    )

    result = storage.recover()

    assert result["nextSeq"] == 14


def test_mid_file_damage_keeps_the_nvs_reservation_floor_in_force(storage: Storage):
    """D-023: the tail is not provably intact, so the floor still applies."""

    storage.write_log(
        record_bytes(10), with_broken_crc(record_bytes(11)), record_bytes(12)
    )

    result = storage.recover(high_water=5000)

    assert result["nextSeq"] == 5001
    assert "NOT provably intact" in result.serial


def test_running_totals_are_reseeded_past_the_damage(storage: Storage):
    """From the last valid record in the file, which is now a later one."""

    storage.write_log(
        record_bytes(10, charge=100, energy=200),
        with_broken_crc(record_bytes(11)),
        record_bytes(12, charge=700, energy=800),
    )

    result = storage.recover()

    assert result["rtcRunCharge"] == 700
    assert result["rtcRunEnergy"] == 800


def test_a_second_boot_does_not_erode_the_log(storage: Storage):
    """Recovery is not allowed to eat one record per boot.

    A refusal that still nudged the file would destroy the same data slowly
    instead of all at once, and a board wakes thousands of times.
    """

    before = b"".join(
        (record_bytes(10), with_broken_crc(record_bytes(11)), record_bytes(12))
    )
    storage.write_log(before)

    for _ in range(3):
        storage.recover()

    assert storage.log_bytes() == before


# ============================================================================
# F. More than one damaged record
# ============================================================================


def test_damage_at_both_the_middle_and_the_tail_is_refused_whole(storage: Storage):
    """The trailing bad record is discardable on its own; the stranded good
    record is not. When the two collide the whole rewrite is refused, because
    preserving evidence beats a partly-correct repair."""

    before = b"".join(
        (
            record_bytes(10),
            with_broken_crc(record_bytes(11)),
            record_bytes(12),
            with_broken_crc(record_bytes(13)),
        )
    )
    storage.write_log(before)

    result = storage.recover()

    assert storage.log_bytes() == before
    assert "NOTHING WAS DISCARDED" in result.serial
    assert result["nextSeq"] == 13


def test_two_separated_mid_file_failures_are_both_counted(storage: Storage):
    storage.write_log(
        record_bytes(10),
        with_broken_crc(record_bytes(11)),
        record_bytes(12),
        with_broken_magic(record_bytes(13)),
        record_bytes(14),
    )

    scan = storage.scan()

    assert scan["validRecords"] == 3
    assert scan["invalidRecords"] == 2
    assert scan["validAfterInvalidRecords"] == 2
    assert scan["firstInvalidOffset"] == 72


# ============================================================================
# The one function that deletes records refuses on its own
# ============================================================================


def test_the_rewrite_refuses_mid_file_damage_even_when_called_directly(
    storage: Storage,
):
    """Defense in depth. autoStorageTruncateToValid() is the only code in the
    firmware that removes stored records, so it does not rely on its caller
    having checked: no accurate report makes a deletion recoverable after it."""

    before = b"".join(
        (record_bytes(10), with_broken_crc(record_bytes(11)), record_bytes(12))
    )
    storage.write_log(before)

    result = storage.force_truncate()

    assert result["truncated"] == 0
    assert storage.log_bytes() == before
    assert "Refusing to rewrite" in result.serial
    assert not storage.temp_exists()


def test_the_rewrite_still_repairs_a_damaged_tail_when_called_directly(
    storage: Storage,
):
    storage.write_log(record_bytes(10), with_broken_crc(record_bytes(11)))

    result = storage.force_truncate()

    assert result["truncated"] == 1
    assert sequences(storage.log_bytes()) == [10]


# ============================================================================
# A log that cannot be read to the end
# ============================================================================


def test_a_short_read_discards_nothing_and_says_so(storage: Storage):
    """An I/O fault is not a torn tail, and must not be repaired as one.

    Staged by telling the firmware the log is 72 bytes longer than it is, so its
    read of the final record comes up short - the same thing the filesystem
    would do on a bad block. Pre-fix this path printed "Treating the remainder
    as an invalid tail" and truncated.
    """

    before = b"".join(record_bytes(seq) for seq in (10, 11))
    storage.write_log(before)

    result = storage.recover(phantom_bytes=RECORD_SIZE)

    assert storage.log_bytes() == before
    assert "Short read while scanning" in result.serial
    assert "nothing will be repaired automatically" in result.serial
    assert "NOTHING WAS DISCARDED" in result.serial
    assert "Discarding" not in result.serial
    assert not storage.temp_exists()


def test_a_short_read_costs_the_log_its_sequence_authority(storage: Storage):
    """Records may exist that were not read, so the NVS floor must apply."""

    storage.write_log(*(record_bytes(seq) for seq in (10, 11)))

    result = storage.recover(high_water=5000, phantom_bytes=RECORD_SIZE)

    assert result["nextSeq"] == 5001
    assert "NOT provably intact" in result.serial


def test_storage_info_calls_an_unreadable_log_unreadable(storage: Storage):
    storage.write_log(*(record_bytes(seq) for seq in (10, 11)))

    result = storage.scan(phantom_bytes=RECORD_SIZE)

    assert result["readError"] == 1


# ============================================================================
# G. An empty log
# ============================================================================


def test_an_empty_log_is_left_alone_and_falls_back_to_the_nvs_floor(
    storage: Storage,
):
    """D-023: an empty log may have been cleared after a sync, so it is never
    the authority."""

    storage.write_log(b"")

    result = storage.recover(high_water=5000)

    assert storage.log_bytes() == b""
    assert result["nextSeq"] == 5001
    assert "NOT provably intact" in result.serial


def test_a_log_that_does_not_exist_yet_is_announced_and_not_created(
    storage: Storage,
):
    result = storage.recover()

    assert not (storage.root / AUTO_LOG_NAME).exists()
    assert "No log file yet" in result.serial
    assert result["nextSeq"] == 1


# ============================================================================
# J. Sequence discontinuity is not corruption
# ============================================================================


def test_a_forward_sequence_gap_is_not_damage(storage: Storage):
    """D-018: gaps are expected after a crash and are preferable to reuse.

    A gap must not cost the log its authority, and must not truncate anything.
    """

    before = b"".join(record_bytes(seq) for seq in (10, 20, 30))
    storage.write_log(before)

    result = storage.recover(high_water=5)

    assert storage.log_bytes() == before
    assert result["nextSeq"] == 31
    assert "provably intact" in result.serial


def test_a_backward_sequence_step_is_reported_but_never_truncated(storage: Storage):
    """Out-of-order means a reuse already happened, so the NVS floor applies.

    It is still not a reason to delete records: every one of them validates.
    """

    before = b"".join(record_bytes(seq) for seq in (10, 20, 15))
    storage.write_log(before)

    result = storage.recover(high_water=5000)

    assert storage.log_bytes() == before
    assert result["nextSeq"] == 5001
    assert "not strictly increasing" in result.serial
    assert "Discarding" not in result.serial


# ============================================================================
# E. Damage that is not a whole number of records - UNRESOLVED
# ============================================================================


def test_misaligned_damage_throws_every_later_record_out_of_frame(storage: Storage):
    """PINNED AS-IS, NOT AS IT SHOULD BE.

    Eight junk bytes spliced into the middle shift every following record off
    the 72-byte grid, so none of them validates and no valid suffix is
    detectable. What follows the damage is therefore indistinguishable from a
    torn tail, and is discarded as one - including the bytes of a record that
    an explicit resynchronization policy could in principle recover.

    This project has no such policy, and inventing one is not part of a
    correctness fix. The behavior is identical before and after this work; the
    case is recorded in docs/BACKLOG.md.
    """

    storage.write_log(record_bytes(10), b"\xde\xad\xbe\xef" * 2, record_bytes(11))

    scan = storage.scan()

    # The bytes of record 11 are present, and not one of them is reachable: no
    # slot on the grid validates, so nothing reads as a stranded valid record.
    assert scan["validRecords"] == 1
    assert scan["validAfterInvalidRecords"] == 0
    assert scan["trailingInvalidRecords"] == 1
    assert scan["partialTail"] == 1

    result = storage.recover()

    assert sequences(storage.log_bytes()) == [10]
    assert "Discarding 80 trailing bytes" in result.serial


# ============================================================================
# LOGGER STORAGE INFO - what an operator is told before deciding anything
# ============================================================================


def test_storage_info_reports_a_clean_log_as_intact(storage: Storage):
    storage.write_log(*(record_bytes(seq) for seq in (10, 11, 12)))

    result = storage.info()

    assert "Valid records:    3" in result.serial
    assert "Tail status:      INTACT" in result.serial
    assert "Invalid records" not in result.serial
    assert "MID-FILE DAMAGE" not in result.serial


def test_storage_info_does_not_let_mid_file_damage_read_as_a_clean_log(
    storage: Storage,
):
    """The tail really is intact. The log really is damaged. Both get said.

    Pre-fix this reported one valid record out of three and no mid-file
    warning at all, so an operator deciding what to do was told less good data
    existed than actually did.
    """

    storage.write_log(
        record_bytes(10),
        with_broken_crc(record_bytes(11)),
        record_bytes(12),
        record_bytes(13),
    )

    result = storage.info()

    assert "Valid records:    3" in result.serial
    assert "Tail status:      INTACT" in result.serial
    assert "Invalid records:  1" in result.serial
    assert "First invalid at: offset 72" in result.serial
    assert "MID-FILE DAMAGE" in result.serial
    assert "2 valid record(s) lie after an invalid one" in result.serial


def test_storage_info_still_names_a_damaged_tail(storage: Storage):
    storage.write_log(record_bytes(10), with_broken_crc(record_bytes(11)))

    result = storage.info()

    assert "Tail status:      INVALID RECORD DETECTED" in result.serial
    assert "MID-FILE DAMAGE" not in result.serial


def test_storage_info_still_names_a_partial_write(storage: Storage):
    storage.write_log(record_bytes(10), b"\x00" * 20)

    result = storage.info()

    assert "Tail status:      PARTIAL WRITE DETECTED" in result.serial


def test_storage_info_never_modifies_the_log(storage: Storage):
    """INFO is an inspection command. It must not repair anything."""

    before = b"".join(
        (record_bytes(10), with_broken_crc(record_bytes(11)), record_bytes(12))
    )
    storage.write_log(before)

    storage.info()

    assert storage.log_bytes() == before
    assert not storage.temp_exists()
