#include <Wire.h>
#include <Preferences.h>
#include <WiFi.h>
#include <LittleFS.h>
#include "esp_sleep.h"
#include "esp_rom_crc.h"

// Version, source revision, and protocol build ID. See the header for what
// each one answers and why they are three separate things.
#include "firmware_version.h"

// INA228 driver: register access, configuration, measurement reads, and the
// accumulator primitives. When to use them stays in this file.
#include "ina228.h"

// Which transport a host has claimed. Leases, sleep, and when autonomous mode
// resumes stay in this file, which calls down into it.
#include "connection.h"

// ============================================================================
// BMW SOLAR LOGGER
// Development firmware
//
// Hardware:
//   Seeed Studio XIAO ESP32-C3
//   Texas Instruments INA228 at I2C address 0x40

//
// Current development goals:
//   1. Keep the system highly observable through Serial.
//   2. Measure solar voltage/current/power once per 60-second interval.
//   3. Use the INA228 hardware CHARGE and ENERGY accumulators.
//   4. Maintain cumulative experiment totals.
//   5. Save experiment state to ESP32 nonvolatile storage (NVS).
//   6. Print machine-readable CSV rows that can later be graphed.
//
// Serial commands:
//   HELP
//   STATUS
//   WIFI ON
//   WIFI OFF
//   WIFI STATUS
//   POWER TEST WIFI
//   POWER TEST SLEEP
//   POWER TEST SLEEP INA OFF
//   POWER TEST STATUS
//   POWER TEST STOP
//   RESET
//   RESET YES
//
// These are bench power-characterization modes. Only ONE may be armed at a
// time. None of them touches the experiment ID, interval number, or running
// charge/energy totals.
//
// The two deep-sleep variants differ only in what the INA228 does during the
// ESP32's sleep phase:
//
//   POWER TEST SLEEP           INA228 stays in continuous conversion.
//                              Hardware CHARGE/ENERGY accumulation continues.
//
//   POWER TEST SLEEP INA OFF   INA228 enters its own documented shutdown
//                              mode first. No conversions, no accumulation.
//                              Solar measurement is sacrificed during sleep.
//
// Neither variant removes INA228 power or touches the XIAO 3V3 rail.
//
// RESET YES deliberately starts a NEW experiment.
// It:
//   - increments experiment_id
//   - resets interval number to 0
//   - resets running charge to 0
//   - resets running energy to 0
//   - resets the INA228 hardware CHARGE/ENERGY accumulators
//   - immediately writes the new zero state to NVS
//
// IMPORTANT TERMINOLOGY:
//
//   INA228 VIN
//       Logic power for the INA228 breakout.
//
//   INA228 VIN+ / VIN-
//       The high-current measurement path through the shunt.
//
// Those are completely different electrical connections.
// ============================================================================

// ============================================================================
// LOGGER TIMING
// ============================================================================

// Once per second we send an instantaneous sensor sample to the laptop.
//
// This does NOT:
//   - reset the INA228 accumulators
//   - increment the interval number
//   - change running charge/energy
//   - write NVS
//
// Its only purpose is high-resolution telemetry for live graphs and future
// analysis.
constexpr unsigned long LIVE_SAMPLE_INTERVAL_MS = 1UL * 1000UL;

// Once per minute we close the INA228 accumulation interval.
//
// This is where we calculate:
//   - interval charge
//   - interval energy
//   - average current
//   - average power
//   - running totals
//
// We also save the recovery checkpoint to NVS here.
constexpr unsigned long MEASUREMENT_INTERVAL_MS = 60UL * 1000UL;

// The human-readable heartbeat can stay at five seconds.
//
// The heartbeat exists mainly to tell us the firmware is alive and healthy.
// The new CSV_SAMPLE output serves a different purpose.
constexpr unsigned long HEARTBEAT_INTERVAL_MS = 5UL * 1000UL;

// We now save after EVERY completed interval.
//
// During development this makes recovery after a power failure easier to
// understand. Later we can revisit flash-write strategy if necessary.
constexpr uint32_t NVS_CHECKPOINT_EVERY_INTERVALS = 1;

// ============================================================================
// ESP32 NONVOLATILE STORAGE
// ============================================================================
//
// Preferences is Arduino's friendly interface to ESP32 NVS.
//
// NVS = Nonvolatile Storage.
//
// It is flash memory inside the ESP32, so values survive:
//   - reset
//   - firmware restart
//   - USB disconnection
//   - complete power loss
//
// We use the namespace:
//
//   solarlog
//
// Think of a namespace as a small named folder containing key/value pairs.
//
// Schema 1 contained:
//   schema
//   interval
//   charge
//   energy
//
// Schema 2 adds:
//   experiment
//
// We deliberately support reading the old schema 1 so flashing this firmware
// does not destroy the state already stored on your ESP32.
// ============================================================================

Preferences preferences;

constexpr char NVS_NAMESPACE[] = "solarlog";

// ESP32 NVS key names are limited to 15 usable characters. "wifi_test_armed"
// is exactly 15. "sleep_test_armed" would be 16 and would be rejected, so the
// deep-sleep test flag uses the shorter "sleep_test_arm".
constexpr char NVS_WIFI_POWER_TEST_KEY[] = "wifi_test_armed";
constexpr char NVS_SLEEP_POWER_TEST_KEY[] = "sleep_test_arm";

// Variant selector for the deep-sleep test. It is only meaningful while
// NVS_SLEEP_POWER_TEST_KEY is true.
//
//   false -> POWER TEST SLEEP           INA228 stays in continuous conversion
//   true  -> POWER TEST SLEEP INA OFF   INA228 enters its own shutdown mode
//
// One armed flag plus one variant flag, rather than two independent armed
// flags, is what makes "both variants armed at once" unrepresentable.
constexpr char NVS_SLEEP_INA_OFF_KEY[] = "sleep_ina_off";

constexpr uint32_t NVS_SCHEMA_VERSION = 2;

// The experiment ID lets CSV rows remain distinguishable even if one Serial
// capture file contains multiple experiments.
//
// Example:
//
//   experiment 1, interval 1
//   experiment 1, interval 2
//   experiment 1, interval 3
//
//   RESET YES
//
//   experiment 2, interval 1
//   experiment 2, interval 2
//
// Filtering experiment_id=2 therefore gives only the second experiment.
uint32_t experimentId = 0;

// Number of fully completed measurement intervals in THIS experiment.
uint32_t completedInterval = 0;

// Cumulative charge and energy for THIS experiment.
double runningCharge_mAh = 0.0;
double runningEnergy_mWh = 0.0;

// ============================================================================
// RUNTIME STATE
// ============================================================================

// millis() returns the number of milliseconds since this ESP32 booted.
//
// We save the millis() value at the beginning of each measurement interval and
// compare it against the current millis() later.
unsigned long intervalStartMs = 0;

unsigned long lastHeartbeatMs = 0;

// millis() timestamp for the most recent high-resolution CSV_SAMPLE sent to
// the laptop. This is independent of the 60-second accounting interval.
unsigned long lastLiveSampleMs = 0;

constexpr unsigned long WIFI_POWER_TEST_PHASE_MS = 30UL * 1000UL;

bool wifiPowerTestArmed = false;
bool wifiPowerTestRunning = false;
uint8_t wifiPowerTestPhase = 0;
unsigned long wifiPowerTestPhaseStartedMs = 0;

// ============================================================================
// DEEP-SLEEP POWER TEST
// ============================================================================
//
// PURPOSE
//
//   Measure XIAO ESP32-C3 supply current while the ESP32 is in TRUE deep
//   sleep and the INA228 breakout remains normally powered from XIAO 3V3.
//
//   The comparison we want is:
//
//     normal awake logger  (already measured at ~28.4 mA)
//     vs
//     ESP32 deep sleep + INA228 still powered
//
//   In this first test we deliberately do NOT power-gate the INA228. VIN,
//   GND, SDA, and SCL all stay connected. Isolating INA228 power is a
//   separate future experiment.
//
//
// PHASE SEQUENCE
//
//   Phase A:  awake for 30 seconds, Wi-Fi OFF, normal INA228 behavior
//   Phase B:  true deep sleep for 30 seconds
//   wake -> Phase A -> repeat
//
//
// WHY THIS IS NOT A NORMAL STATE MACHINE
//
//   ESP32 deep sleep does not resume after esp_deep_sleep_start(). The chip
//   reboots and setup() runs again from the top. There is therefore no
//   "phase B code" that runs during sleep, and no code path that returns
//   from sleep into loop().
//
//   Every wake is a fresh boot. The test state machine is consequently
//   split across the reset boundary:
//
//     setup()  decides "am I starting an awake phase, and why?"
//     loop()   decides "has the awake phase lasted long enough to sleep?"
//
constexpr unsigned long SLEEP_POWER_TEST_AWAKE_MS = 30UL * 1000UL;
constexpr uint64_t SLEEP_POWER_TEST_SLEEP_US = 30ULL * 1000000ULL;

// ----------------------------------------------------------------------------
// WHY THE CYCLE COUNTER LIVES IN RTC MEMORY INSTEAD OF NVS
// ----------------------------------------------------------------------------
//
// NVS is flash. Flash has finite erase endurance. This test sleeps every 30
// seconds, which is 2,880 write cycles per day. Writing a counter to flash
// that often would burn endurance for a value that has no long-term meaning.
//
// RTC slow memory has exactly the lifetime we want:
//
//   retained  across deep sleep  (which is the whole point)
//   lost      on power loss      (which ends the bench run anyway)
//
// So the split is deliberate:
//
//   NVS  ->  "is the deep-sleep test armed?"    (must survive power loss)
//   RTC  ->  "which cycle are we on?"           (only meaningful within one
//                                                continuous battery run)
//
// RTC memory contents are UNDEFINED after a power-on reset, so a magic value
// guards it. Without the guard we would read whatever bits happened to be in
// RAM and report a fabricated cycle number.
// ----------------------------------------------------------------------------

constexpr uint32_t SLEEP_POWER_TEST_RTC_MAGIC = 0x5A1EEB01;

RTC_DATA_ATTR uint32_t rtcSleepTestMagic = 0;
RTC_DATA_ATTR uint32_t rtcSleepTestCycle = 0;

bool sleepPowerTestArmed = false;
bool sleepPowerTestRunning = false;
unsigned long sleepPowerTestAwakeStartedMs = 0;

// Which variant of the deep-sleep test is armed. Only meaningful while
// sleepPowerTestArmed is true.
bool sleepPowerTestInaOff = false;

// True only between a verified INA228 shutdown and the deep sleep that
// follows it. Nothing may present INA228 register contents as fresh
// measurements while this is set, because no conversions are running.
//
// In practice deep sleep follows within microseconds, so this exists to make
// the invariant explicit rather than to depend on control flow being fast.
bool inaShutdownActive = false;

// ============================================================================
// AUTONOMOUS LOGGER BENCH TEST
// ============================================================================
//
// A BENCH TEST, not production behavior. It proves one cycle of the
// autonomous architecture designed in docs/STORAGE_SYNC_DESIGN.md:
//
//   INA228 keeps converting through ESP32 deep sleep
//     -> timer wake
//     -> READ accumulated CHARGE and ENERGY *before* anything can reset them
//     -> read a V/I/P/temperature snapshot
//     -> append one durable binary record to LittleFS
//     -> only then reset the accumulators for the next interval
//     -> deep sleep again
//
// What this test deliberately does NOT do: Bluetooth, Wi-Fi, sync, ACK,
// pruning, or a production ring buffer.
//
//
// ACCOUNTING ISOLATION — this is the important safety property.
//
// The running charge and energy totals written into these records are
// TEST-LOCAL. They live in RTC memory and in the durable log, and they are
// recovered from the log on a cold boot. They are NOT the NVS experiment
// totals.
//
// This path never calls saveCheckpoint() and never modifies experimentId,
// completedInterval, runningCharge_mAh, or runningEnergy_mWh. The record
// carries experimentId only as read-only context, so a record can be
// attributed to the experiment that was current when it was written.
//
// Design decision B from the task brief: isolated test accounting, existing
// experiment totals frozen. Modifying live experiment accounting from a bench
// test would risk double-counting or erasing real charge, and the test does
// not need it.
// ============================================================================

// The single authority for autonomous cadence. Everything derives from this;
// no other 60-second constant exists in the autonomous path.
constexpr uint32_t AUTO_INTERVAL_SECONDS_DEFAULT = 60;
constexpr uint32_t AUTO_INTERVAL_SECONDS_MIN = 10;
constexpr uint32_t AUTO_INTERVAL_SECONDS_MAX = 3600;

uint32_t autonomousIntervalSeconds = AUTO_INTERVAL_SECONDS_DEFAULT;

// NVS keys. The 15-character limit applies to all of these.
constexpr char NVS_AUTO_TEST_KEY[] = "auto_test_arm";
constexpr char NVS_AUTO_INTERVAL_KEY[] = "auto_int_s";
constexpr char NVS_AUTO_SEQ_HW_KEY[] = "auto_seq_hw";
constexpr char NVS_BOOT_ID_KEY[] = "boot_id";

constexpr char AUTO_LOG_PATH[] = "/auto.bin";
constexpr char AUTO_LOG_TMP_PATH[] = "/auto.tmp";

constexpr uint16_t AUTO_RECORD_MAGIC = 0xB5A5;
constexpr uint8_t AUTO_RECORD_VERSION = 1;

// Sequence numbers are reserved from NVS in blocks so that a crash costs a
// gap rather than a reuse. Gaps are visible and harmless; duplicates would
// silently corrupt a host's record of what happened.
constexpr uint32_t AUTO_SEQ_BLOCK = 64;

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
// Cold-boot maintenance window
// ----------------------------------------------------------------------------
//
// Discovered on hardware 2026-09-11. An armed autonomous board is very hard to
// stop over USB, because the only Serial command parser runs in loop(), and an
// armed cold boot returns straight from setup() into deep sleep without ever
// reaching loop(). A perfectly timed LOGGER TEST STOP was therefore never
// parsed, and tools/send.sh reported success for bytes the firmware never read.
//
// This window is the guaranteed recovery mechanism: on a true cold boot the
// board stays awake, parsing commands, for a known length of time before it
// resumes autonomous sleeping.
//
// It deliberately does NOT apply to a normal timer wake. Timer wakes stay as
// short as possible, because awake time dominates the power budget.
//
// Development value. Shorten it once a management transport exists.
// ----------------------------------------------------------------------------
constexpr uint32_t AUTONOMOUS_COLD_BOOT_MAINTENANCE_MS = 15000;

// How often the window prints that it is open and how much is left.
constexpr uint32_t AUTONOMOUS_MAINTENANCE_TICK_MS = 5000;

// ----------------------------------------------------------------------------
// USB rendezvous and host session lease
// ----------------------------------------------------------------------------
//
// The timer wake itself was proved working on hardware: durable records grew
// from 65 to 99 while the board was unreachable over USB. What was missing was
// any opportunity for a host to take the board over. The optimized wake was
// short enough that macOS could not reliably enumerate the USB device, open it,
// and send a command before the board slept again.
//
// So every successful timer wake now offers a short rendezvous window. A host
// that gets in claims the wake with LOGGER SESSION HOLD and the board stays
// awake as an ordinary tethered logger until the host releases or stops
// renewing its lease.
//
//   SLEEP -> TIMER WAKE -> MEASURE/STORE -> USB RENDEZVOUS
//                                             -> no claim -> SLEEP
//                                             -> HOLD -> HOST SESSION
//                                                          -> KEEPALIVE...
//                                                          -> RELEASE
//                                                          -> or lease expiry
//                                                          -> SLEEP
//
// DEVELOPMENT VALUES. A 3-second rendezvous on every 60-second wake is a large
// fraction of the duty cycle and will cost real power. That is accepted while
// the behavior is being proved. Making it configurable, less frequent than
// every wake, or absent in a deployment mode all come later.
// ----------------------------------------------------------------------------
//
// TEN SECONDS, deliberately. The blocking question is whether a timer wake
// makes the USB device usable to macOS at all, with no reset, no BOOT button,
// no power cycle and no replug. A window long enough to be unmissable answers
// that; a window tuned for power would confuse "USB never came up" with "USB
// came up too briefly". Power optimization comes after the behavior is proved.
constexpr uint32_t AUTONOMOUS_USB_RENDEZVOUS_MS = 10000;

// How long a host session survives without a KEEPALIVE. Chosen against the
// logger's 5-second keepalive cadence: three missed keepalives end the session,
// so a brief host stall does not drop it but a crashed or unplugged host cannot
// hold the board awake indefinitely.
constexpr uint32_t AUTONOMOUS_HOST_LEASE_MS = 15000;

// Machine protocol lines get a short retry window because HWCDC writes with
// setTxTimeoutMs(0) can report a short write when its ring is full. This is a
// hard cap for protocol bytes only; ordinary diagnostics remain nonblocking.
constexpr uint32_t COMMAND_PROTOCOL_WRITE_TIMEOUT_MS = 100;

// RELEASE tears down the USB transport it uses for its own acknowledgement.
// Give the HWCDC endpoint a short, hard-bounded opportunity to deliver the
// echo and outcome before entering deep sleep. This is deliberately not a
// Serial timeout or an unbounded flush, so lease and state-machine deadlines
// remain independent of host availability.
constexpr uint32_t SESSION_RELEASE_ACK_GRACE_MS = 250;

// Rendezvous progress line cadence. Frequent enough to show the window is open,
// rare enough not to flood a host that is still enumerating.
constexpr uint32_t AUTONOMOUS_RENDEZVOUS_TICK_MS = 1000;

// Where the autonomous logger currently is. Printed on every transition,
// because a state machine that changes silently is one nobody can debug.
enum AutoState : uint8_t
{
	AUTO_STATE_IDLE,									 // not armed; ordinary tethered logger
	AUTO_STATE_COLD_BOOT_MAINTENANCE,	 // armed, cold-boot recovery window open
	AUTO_STATE_TIMER_WAKE_MEASUREMENT, // armed, measuring and storing
	AUTO_STATE_USB_RENDEZVOUS,				 // armed, offering the wake to a host
	AUTO_STATE_HOST_SESSION,					 // armed, but a host holds the board awake
	AUTO_STATE_HOST_SESSION_RELEASED,	 // host let go; returning to autonomous
	AUTO_STATE_HOST_LEASE_EXPIRED,		 // host stopped renewing; returning
	AUTO_STATE_DEEP_SLEEP_PENDING,		 // about to sleep
};

// Which path is about to enter deep sleep. Only the timer-wake path produces
// a number that may be compared against other timer wakes; the others include
// full initialization and are labeled so they are never read as wake timings.
enum AutoSleepPath : uint8_t
{
	AUTO_SLEEP_TIMER_WAKE,	 // the optimized path: setup() -> wake cycle -> sleep
	AUTO_SLEEP_COLD_BOOT,		 // power-on/reset resume, includes init + window
	AUTO_SLEEP_ARM_COMMAND,	 // operator typed LOGGER TEST AUTONOMOUS
	AUTO_SLEEP_RTC_LOST,		 // timer wake whose RTC session state was gone
	AUTO_SLEEP_HOST_RELEASE, // a host session ended and handed the board back
};

// How the host-session -> autonomous handoff ended. Each failure names the
// stage that failed, so RELEASE and lease expiry can say which one (D-044).
enum HandoffResult : uint8_t
{
	HANDOFF_PREPARED,						 // interval closed, autonomous state valid, baseline set
	HANDOFF_INTERVAL_NOT_CLOSED, // closeMeasurementInterval() failed
	HANDOFF_STORAGE_UNAVAILABLE, // RTC state needed rebuilding; storage would not mount
};

// How LOGGER SESSION HOLD ended. Two different refusals, because a host has to
// act differently on them (D-044):
//
//   HOLD_NOT_ARMED      autonomous mode is off; the board stays awake anyway
//                       and no lease is needed. Answered REFUSED.
//   HOLD_SLEEP_PENDING  the board is already committed to a deferred autonomous
//                       sleep; claim it at the next rendezvous. Answered ERROR.
enum HoldResult : uint8_t
{
	HOLD_GRANTED,
	HOLD_NOT_ARMED,
	HOLD_SLEEP_PENDING,
};

// What the deadline scheduler decided (D-032), from planAutonomousSleep().
// Declared up here because the Arduino preprocessor puts generated prototypes
// ahead of the first function definition, and one of them returns this.
struct AutoSleepPlan
{
	uint32_t sleepMs;					// the commanded deep sleep
	uint32_t intervalStartMs; // the boundary to keep; moves only on an overrun
	uint32_t wakeElapsedMs;		// the retained session clock for the next wake
	uint32_t overrunMs;				// how far past the deadline, when overran
	bool overran;
};

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

// Result of one full scan of the durable log. Declared here, next to the
// record, rather than beside the scan function: the Arduino sketch
// preprocessor emits generated prototypes ahead of the first function
// definition, so any type named in a prototype must already exist by then.
struct AutoLogScan
{
	bool mounted;
	bool fileExists;
	uint32_t validRecords;
	uint32_t firstSeq;
	uint32_t lastSeq;
	size_t fileBytes;
	size_t validBytes;
	size_t trailingBytes;		 // bytes past the last valid record
	uint32_t invalidRecords; // whole-sized records that failed validation
	bool partialTail;				 // file size is not a whole number of records
	bool seqOutOfOrder;
	int64_t lastRunChargeUAh;
	int64_t lastRunEnergyUWh;
};

// ----------------------------------------------------------------------------
// RTC-retained autonomous state
// ----------------------------------------------------------------------------
//
// Retained across deep sleep, lost on power loss, which is exactly the
// lifetime of a measurement session. Guarded by a magic value because RTC
// memory contents are undefined after a cold boot.
//
// Everything here that must survive power loss lives in NVS or in the durable
// log instead.
// ----------------------------------------------------------------------------

constexpr uint32_t AUTO_RTC_MAGIC = 0xA07011E5;

RTC_DATA_ATTR uint32_t rtcAutoMagic = 0;
RTC_DATA_ATTR uint32_t rtcAutoBootId = 0;
RTC_DATA_ATTR uint32_t rtcAutoSessionElapsedMs = 0;
RTC_DATA_ATTR uint32_t rtcAutoIntervalStartMs = 0;
RTC_DATA_ATTR uint32_t rtcAutoNextSeq = 0;
RTC_DATA_ATTR uint32_t rtcAutoSeqHighWater = 0;
RTC_DATA_ATTR int64_t rtcAutoRunChargeUAh = 0;
RTC_DATA_ATTR int64_t rtcAutoRunEnergyUWh = 0;
RTC_DATA_ATTR uint32_t rtcAutoCycleCount = 0;
RTC_DATA_ATTR uint32_t rtcAutoCommandedSleepMs = 0;

bool autonomousTestArmed = false;
bool autonomousTestRunning = false;
bool autoStorageMounted = false;

// Set as soon as a stop is ASKED FOR, before it is known to have worked.
//
// stopAutonomousTest() clears autonomousTestArmed only after the NVS write
// succeeds, which is correct. But that leaves the two cases "no stop was sent"
// and "a stop was sent and could not be persisted" looking identical to the
// maintenance window, and it would resume sleeping in both. Deep sleep after
// an explicit stop request is the same class of bug this window exists to fix.
//
// Plain RAM, so a boot clears it. The persisted flag is the only thing that
// survives, and it is still the authority on whether the test is armed.
bool autonomousStopRequested = false;

// ----------------------------------------------------------------------------
// Host session lease
// ----------------------------------------------------------------------------
//
// An explicit software lease, deliberately NOT Serial.isConnected() or
// isPlugged(). Those were audited on 2026-09-11: isPlugged() is a SOF watchdog
// the ESP32 core's own source says "is known to flap even on a healthy link",
// and isConnected() reads false whenever no terminal holds the port open even
// with USB attached. Neither can decide whether the board may sleep.
//
// A lease is a positive claim from software that is actually talking to us, and
// it expires on its own, so a crashed host, a yanked cable, or a laptop going
// to sleep all resolve without any cooperation from the host.
//
// Plain RAM. A host session is by definition a property of one awake period.
// ----------------------------------------------------------------------------
bool hostSessionHeld = false;
uint32_t hostLeaseDeadlineMs = 0;
uint32_t hostSessionStartedMs = 0;
uint32_t hostKeepaliveCount = 0;

// When the lease was last taken or renewed, so STATUS can report how long the
// host has been quiet rather than only how long is left.
uint32_t hostLeaseRenewedMs = 0;

// True once HOLD has established the tethered measurement baseline, so the
// ordinary initialization further down setup() does not reset the accumulators
// a second time and throw the session's charge away.
bool hostSessionBaselineEstablished = false;

// How many ordinary 60-second intervals actually closed during this session.
//
// Counted directly rather than derived from completedInterval, and that is the
// whole point. The first version captured completedInterval at the claim and
// subtracted later. But HOLD runs during the USB rendezvous, which is BEFORE
// loadCheckpoint() restores completedInterval from NVS, so the baseline was
// always 0 and the difference was simply the restored interval number: a
// 25-second session reported 2350 intervals closed.
//
// That is the same class of defect as D-024, introduced while fixing D-024.
// A counter incremented at the actual event cannot be wrong about ordering.
uint32_t hostSessionIntervalCloses = 0;

// Set by LOGGER SESSION HOLD during the rendezvous window so the wake cycle
// knows to stay awake instead of sleeping.
bool hostClaimedRendezvous = false;

// Deferred autonomous deep sleep, so a command that destroys its own transport
// can deliver CMD_RESULT before the transport disappears (D-035).
//
// Two commands need it and they are not the same command, which is why the path
// travels with the deadline rather than being assumed:
//
//   LOGGER SESSION RELEASE   completes the accounting handoff, then sleeps
//   LOGGER AUTONOMOUS ON     arms, then sleeps into the first interval
//
// Named for the mechanism rather than for RELEASE, because a value whose name
// says one thing while it carries another is how the next reader gets it wrong.
bool pendingAutonomousSleep = false;
uint32_t pendingAutonomousSleepDeadlineMs = 0;
AutoSleepPath pendingAutonomousSleepPath = AUTO_SLEEP_HOST_RELEASE;

AutoState autonomousState = AUTO_STATE_IDLE;

// Sub-timings from the last autoStorageAppend(), in microseconds. Split out
// because open, write, and flush+close have very different costs on LittleFS
// and a single combined number hides which one dominates the wake.
uint32_t autoAppendOpenUs = 0;
uint32_t autoAppendWriteUs = 0;
uint32_t autoAppendFlushCloseUs = 0;

// Set when a power test ends but the INA228 accumulators could not be cleared.
// Accounting must stay suspended in that case: the accumulators hold charge
// from an unaccounted window of unknown length, and folding it into the next
// interval would produce a wrong average current and average power that look
// exactly like right ones.
//
// This is ordinary RAM, not RTC memory or NVS, so a reset clears it. That is
// intended: setup() clears the accumulators on every boot.
bool intervalAccountingBlocked = false;

// Captured once, at the very top of setup(), before anything else can run.
esp_sleep_wakeup_cause_t bootWakeupCause = ESP_SLEEP_WAKEUP_UNDEFINED;

// micros() sampled on the first line of setup(). Every "total duration" figure
// the autonomous path prints is measured from here, so it covers the whole
// awake period rather than starting partway through it.
//
// It does NOT include the bootloader: micros() is already non-zero when setup()
// begins, and that offset is printed alongside the total rather than hidden.
uint32_t setupEntryMicros = 0;

// millis() at the moment the host handoff set rtcAutoSessionElapsedMs to the
// session time of that moment. The scheduler adds only the time since then on
// the AUTO_SLEEP_HOST_RELEASE path (D-046). Only the handoff writes it, and it
// is meaningful only on the boot that wrote it, so plain RAM is right.
uint32_t hostHandoffAtMs = 0;

// ============================================================================
// WHICH POWER TEST IS ARMED
// ============================================================================
//
// The Wi-Fi test and the deep-sleep test are mutually exclusive. They are
// stored under two separate NVS keys rather than one shared mode key so that
// the existing, already-deployed wifi_test_armed key keeps working exactly as
// documented in DECISIONS.md D-006.
//
// Two independent flags make one invalid combination representable: both
// armed at once. That state is never created by this firmware, but if it ever
// appears we refuse to guess which test the user meant.
// ============================================================================

enum PowerTestMode : uint8_t
{
	POWER_TEST_NONE = 0,
	POWER_TEST_WIFI = 1,
	POWER_TEST_SLEEP = 2,
	POWER_TEST_CONFLICT = 3,
};

// armedPowerTestMode() and intervalAccountingSuspended() are defined further
// down, alongside the other power-test functions.
//
// That placement used to be load-bearing. The Arduino sketch preprocessor
// inserts its generated function prototypes immediately before the FIRST
// function definition in the file, and a function defined here would have
// pushed that insertion point above "struct SensorReading", which the sketch
// then defined further down. SensorReading now comes from ina228.h, included
// at the top, and every type the sketch defines is above this line, so the
// hazard is gone. The functions were left where they are rather than moved
// as part of the INA228 extraction.

// This is OUR command-building buffer.
//
// Serial data does not arrive as a complete String.
//
// If you type:
//
//   STATUS<Enter>
//
// the ESP32 receives individual characters:
//
//   'S'
//   'T'
//   'A'
//   'T'
//   'U'
//   'S'
//   '\n'
//
// We append the ordinary characters to this String.
//
// When '\n' or '\r' arrives, we know the command is complete and send the
// finished String to processCommand().
String serialCommandBuffer;

bool writeProtocolLine(const String &line, uint32_t timeoutMs);

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================
//
// These seventeen functions are called above the line where they are defined.
// The build works without this block only because Arduino CLI synthesizes
// prototypes into the generated .ino.cpp it actually compiles - which means
// the file a person edits is not, by itself, a valid C++ translation unit.
//
// That gap is not free. An editor reading solar-logger.ino directly reports
// every one of these calls as an undeclared identifier, and no amount of
// include-path configuration fixes it, because the declarations genuinely are
// not there. Declaring them here makes the sketch stand on its own as ordinary
// C++, so the source, the compiler, and the editor all see the same thing.
//
// These are declarations only. They emit no code and change no behavior.
//
// tools/intellisense.sh compiles this file standalone and fails loudly if a
// new forward reference is added without a declaration here, so the list
// cannot quietly fall behind.
//
// Keep them sorted, and delete each one as its owner moves into a real module
// with a real header - see the modularization plan in docs/BACKLOG.md.

bool armAutonomousTest();
const char *autoStateName(AutoState state);
bool autonomousUsbRendezvous(bool cycleOk);
bool clearStorage();
bool dumpStorage();
HoldResult hostSessionHold();
void hostSessionKeepalive();
bool hostSessionRelease();
void printAutonomousStatus();
void printDeprecatedCommand(const char *oldName, const char *newName);
void printHostSessionStatus();
bool printStorageInfo();
void printUsbPresence(const char *label);
bool saveAutonomousInterval(uint32_t seconds);
void setAutoState(AutoState next, const char *reason);
bool stopAllPowerTests();
bool stopAutonomousTest();

// ============================================================================
// HIGH-RESOLUTION LIVE SAMPLE
// ============================================================================
//
// This creates one instantaneous telemetry point for the laptop.
//
// CSV_SAMPLE is different from CSV_DATA:
//
//   CSV_SAMPLE
//       High-resolution instantaneous measurement.
//       Currently generated once per second.
//
//   CSV_DATA
//       Authoritative 60-second interval summary.
//       Includes accumulated charge, energy, averages, and running totals.
//
// Reading the live VBUS/CURRENT/POWER registers does NOT reset or interfere
// with the INA228 CHARGE or ENERGY accumulators.
//
void printLiveSample()
{
	// CSV_SAMPLE carries elapsed_seconds, which is derived from
	// completedInterval and millis(). Both are meaningless during the
	// deep-sleep test: completedInterval is frozen because accounting is
	// suspended, and millis() restarts at zero on every wake. Emitting rows
	// anyway would write a sawtooth elapsed_seconds into the live experiment's
	// samples.csv under an unchanged experiment_id.
	//
	// Suppressing the machine-readable row is the honest option. The
	// human-readable heartbeat keeps printing real INA228 values every five
	// seconds, so Serial observability is unaffected.
	//
	// Note that this checks sleepPowerTestRunning, NOT the broader
	// intervalAccountingSuspended(). The difference is deliberate: in the
	// accounting-blocked state the board is awake continuously, so
	// elapsed_seconds still advances monotonically and the samples are real.
	// Only repeated deep sleep makes the value jump backwards.
	if (sleepPowerTestRunning)
	{
		return;
	}

	SensorReading reading;

	// readSensor() fills the SensorReading struct with the latest values
	// currently available from the INA228.
	//
	// false means:
	//   Do not print the verbose VSHUNT diagnostic block for every 1-second
	//   sample. That would create a huge amount of Serial output.
	if (!readSensor(reading, false))
	{
		Serial.println(
				"[ERROR] LIVE SAMPLE failed because INA228 read failed.");
		return;
	}

	// We do not yet have a real wall-clock timestamp.
	//
	// Instead, create an elapsed time within the experiment.
	//
	// completedInterval:
	//   Number of complete 60-second intervals already finished.
	//
	// millis() - intervalStartMs:
	//   Number of milliseconds elapsed in the CURRENT interval.
	//
	// Example:
	//
	//   3 complete intervals = 180 seconds
	//   17.2 seconds into interval 4
	//
	//   experimentElapsedSeconds = 197.2 seconds
	//
	double experimentElapsedSeconds =
			completedInterval * 60.0 +
			(millis() - intervalStartMs) / 1000.0;

	// Machine-readable high-resolution telemetry.
	//
	// Format:
	//
	// CSV_SAMPLE,
	// experiment_id,
	// elapsed_seconds,
	// voltage_V,
	// current_mA,
	// power_mW,
	// temperature_C
	//
	Serial.print("CSV_SAMPLE,");

	Serial.print(experimentId);

	Serial.print(',');
	Serial.print(experimentElapsedSeconds, 3);

	Serial.print(',');
	Serial.print(reading.voltage_V, 6);

	Serial.print(',');
	Serial.print(reading.current_mA, 6);

	Serial.print(',');
	Serial.print(reading.power_mW, 6);

	Serial.print(',');
	Serial.println(reading.temperature_C, 4);
}

// ============================================================================
// SAVE CURRENT STATE TO NVS
// ============================================================================

bool saveCheckpoint()
{
	Serial.println();

	Serial.print("[NVS] Writing checkpoint for experiment ");
	Serial.print(experimentId);
	Serial.print(", interval ");
	Serial.println(completedInterval);

	// false means open the namespace READ/WRITE.
	if (!preferences.begin(NVS_NAMESPACE, false))
	{
		Serial.println(
				"[ERROR] Could not open NVS namespace for writing.");
		return false;
	}

	bool ok = true;

	Serial.print("[NVS] Writing schema = ");
	Serial.println(NVS_SCHEMA_VERSION);

	if (preferences.putUInt(
					"schema",
					NVS_SCHEMA_VERSION) != sizeof(uint32_t))
	{

		Serial.println("[ERROR] Failed to write NVS key: schema");
		ok = false;
	}

	Serial.print("[NVS] Writing experiment = ");
	Serial.println(experimentId);

	if (preferences.putUInt(
					"experiment",
					experimentId) != sizeof(uint32_t))
	{

		Serial.println("[ERROR] Failed to write NVS key: experiment");
		ok = false;
	}

	Serial.print("[NVS] Writing interval = ");
	Serial.println(completedInterval);

	if (preferences.putUInt(
					"interval",
					completedInterval) != sizeof(uint32_t))
	{

		Serial.println("[ERROR] Failed to write NVS key: interval");
		ok = false;
	}

	Serial.print("[NVS] Writing charge = ");
	Serial.print(runningCharge_mAh, 6);
	Serial.println(" mAh");

	if (preferences.putDouble(
					"charge",
					runningCharge_mAh) != sizeof(double))
	{

		Serial.println("[ERROR] Failed to write NVS key: charge");
		ok = false;
	}

	Serial.print("[NVS] Writing energy = ");
	Serial.print(runningEnergy_mWh, 6);
	Serial.println(" mWh");

	if (preferences.putDouble(
					"energy",
					runningEnergy_mWh) != sizeof(double))
	{

		Serial.println("[ERROR] Failed to write NVS key: energy");
		ok = false;
	}

	// Flush/close the Preferences namespace.
	preferences.end();

	if (ok)
	{
		Serial.println(
				"*** NVS CHECKPOINT SAVED SUCCESSFULLY ***");
	}
	else
	{
		Serial.println(
				"*** ERROR: NVS CHECKPOINT WAS NOT FULLY SAVED ***");
	}

	return ok;
}

// The Wi-Fi power-test flag is stored under its own key. It is deliberately
// separate from the experiment checkpoint so arming or stopping this bench
// test cannot alter experiment totals, interval numbers, or experiment IDs.
bool saveWifiPowerTestArmed(bool armed)
{
	Serial.print("[NVS] Writing ");
	Serial.print(NVS_WIFI_POWER_TEST_KEY);
	Serial.print(" = ");
	Serial.println(armed ? "true" : "false");

	if (!preferences.begin(NVS_NAMESPACE, false))
	{
		Serial.println("[ERROR] Could not open NVS for Wi-Fi power-test flag.");
		return false;
	}

	size_t written = preferences.putBool(NVS_WIFI_POWER_TEST_KEY, armed);
	preferences.end();

	if (written != sizeof(bool))
	{
		Serial.println("[ERROR] Failed to write Wi-Fi power-test NVS flag.");
		return false;
	}

	Serial.println("[NVS] Wi-Fi power-test flag saved: ALL OK");
	return true;
}

bool loadWifiPowerTestArmed()
{
	if (!preferences.begin(NVS_NAMESPACE, true))
	{
		// The message below promises a value, so actually assign it. Leaving the
		// global untouched here would make the printed claim depend on whatever
		// the variable already held.
		wifiPowerTestArmed = false;
		Serial.println(
				"[NVS] Wi-Fi power-test flag unavailable; defaulting to not armed.");
		return true;
	}

	if (!preferences.isKey(NVS_WIFI_POWER_TEST_KEY))
	{
		preferences.end();
		wifiPowerTestArmed = false;
		Serial.println("[NVS] Wi-Fi power-test flag: not armed.");
		return true;
	}

	wifiPowerTestArmed = preferences.getBool(NVS_WIFI_POWER_TEST_KEY, false);
	preferences.end();

	Serial.print("[NVS] Wi-Fi power-test flag: ");
	Serial.println(wifiPowerTestArmed ? "armed" : "not armed");
	return true;
}

// The deep-sleep power-test flag uses its own NVS key for the same reason the
// Wi-Fi flag does: arming or stopping a bench test must never be able to
// alter experiment totals, interval numbers, or the experiment ID.
bool saveSleepPowerTestArmed(bool armed)
{
	Serial.print("[NVS] Writing ");
	Serial.print(NVS_SLEEP_POWER_TEST_KEY);
	Serial.print(" = ");
	Serial.println(armed ? "true" : "false");

	if (!preferences.begin(NVS_NAMESPACE, false))
	{
		Serial.println("[ERROR] Could not open NVS for deep-sleep test flag.");
		return false;
	}

	size_t written = preferences.putBool(NVS_SLEEP_POWER_TEST_KEY, armed);
	preferences.end();

	if (written != sizeof(bool))
	{
		Serial.println("[ERROR] Failed to write deep-sleep test NVS flag.");
		return false;
	}

	Serial.println("[NVS] Deep-sleep test flag saved: ALL OK");
	return true;
}

bool loadSleepPowerTestArmed()
{
	if (!preferences.begin(NVS_NAMESPACE, true))
	{
		sleepPowerTestArmed = false;
		Serial.println(
				"[NVS] Deep-sleep test flag unavailable; defaulting to not armed.");
		return true;
	}

	if (!preferences.isKey(NVS_SLEEP_POWER_TEST_KEY))
	{
		preferences.end();
		sleepPowerTestArmed = false;
		Serial.println("[NVS] Deep-sleep test flag: not armed.");
		return true;
	}

	sleepPowerTestArmed = preferences.getBool(NVS_SLEEP_POWER_TEST_KEY, false);
	preferences.end();

	Serial.print("[NVS] Deep-sleep test flag: ");
	Serial.println(sleepPowerTestArmed ? "armed" : "not armed");
	return true;
}

bool saveSleepPowerTestInaOff(bool inaOff)
{
	Serial.print("[NVS] Writing ");
	Serial.print(NVS_SLEEP_INA_OFF_KEY);
	Serial.print(" = ");
	Serial.println(inaOff ? "true" : "false");

	if (!preferences.begin(NVS_NAMESPACE, false))
	{
		Serial.println("[ERROR] Could not open NVS for deep-sleep variant flag.");
		return false;
	}

	size_t written = preferences.putBool(NVS_SLEEP_INA_OFF_KEY, inaOff);
	preferences.end();

	if (written != sizeof(bool))
	{
		Serial.println("[ERROR] Failed to write deep-sleep variant NVS flag.");
		return false;
	}

	Serial.println("[NVS] Deep-sleep variant flag saved: ALL OK");
	return true;
}

bool loadSleepPowerTestInaOff()
{
	if (!preferences.begin(NVS_NAMESPACE, true))
	{
		sleepPowerTestInaOff = false;
		Serial.println(
				"[NVS] Deep-sleep variant flag unavailable; defaulting to INA "
				"continuous.");
		return true;
	}

	if (!preferences.isKey(NVS_SLEEP_INA_OFF_KEY))
	{
		preferences.end();
		sleepPowerTestInaOff = false;
		Serial.println("[NVS] Deep-sleep variant flag: INA continuous.");
		return true;
	}

	sleepPowerTestInaOff = preferences.getBool(NVS_SLEEP_INA_OFF_KEY, false);
	preferences.end();

	Serial.print("[NVS] Deep-sleep variant flag: ");
	Serial.println(sleepPowerTestInaOff ? "INA shutdown" : "INA continuous");
	return true;
}

// One place decides how a sleep-test variant is named, so the banner, STATUS,
// and stop messages cannot drift apart.
const char *sleepPowerTestVariantName()
{
	return sleepPowerTestInaOff ? "INA shutdown" : "INA continuous";
}

// ============================================================================
// LOAD STATE FROM NVS
// ============================================================================
//
// This understands BOTH:
//
//   schema 1
//   schema 2
//
// Your currently stored data is schema 1.
//
// When schema 1 is encountered, we load its interval/charge/energy exactly as
// before and assign:
//
//   experimentId = 0
//
// The first RESET YES after installing this version will therefore create:
//
//   experimentId = 1
// ============================================================================

bool loadCheckpoint()
{
	Serial.println();
	Serial.println("[NVS] Beginning checkpoint load...");

	Serial.print("[NVS] Opening namespace \"");
	Serial.print(NVS_NAMESPACE);
	Serial.println("\" in READ ONLY mode...");

	// true = READ ONLY.
	if (!preferences.begin(NVS_NAMESPACE, true))
	{
		Serial.println(
				"[WARNING] NVS namespace does not exist yet. Starting from zero.");

		experimentId = 0;
		completedInterval = 0;
		runningCharge_mAh = 0.0;
		runningEnergy_mWh = 0.0;

		return true;
	}

	Serial.println("[NVS] Namespace opened.");

	if (!preferences.isKey("schema"))
	{
		Serial.println(
				"[WARNING] NVS contains no schema key. Starting from zero.");

		preferences.end();

		experimentId = 0;
		completedInterval = 0;
		runningCharge_mAh = 0.0;
		runningEnergy_mWh = 0.0;

		return true;
	}

	uint32_t schema =
			preferences.getUInt("schema", 0);

	Serial.print("[NVS] Stored schema = ");
	Serial.println(schema);

	// --------------------------------------------------------------------------
	// LEGACY SCHEMA 1
	// --------------------------------------------------------------------------

	if (schema == 1)
	{
		Serial.println(
				"[NVS] Legacy schema 1 detected.");

		Serial.println(
				"[NVS] Loading old interval/charge/energy values.");

		Serial.println(
				"[NVS] Legacy data will be treated as experiment 0.");

		if (!preferences.isKey("interval") ||
				!preferences.isKey("charge") ||
				!preferences.isKey("energy"))
		{

			Serial.println(
					"[ERROR] Schema 1 checkpoint is incomplete.");

			preferences.end();
			return false;
		}

		experimentId = 0;

		completedInterval =
				preferences.getUInt("interval", 0);

		runningCharge_mAh =
				preferences.getDouble("charge", 0.0);

		runningEnergy_mWh =
				preferences.getDouble("energy", 0.0);

		preferences.end();

		Serial.print("[NVS] experiment = ");
		Serial.println(experimentId);

		Serial.print("[NVS] interval = ");
		Serial.println(completedInterval);

		Serial.print("[NVS] charge = ");
		Serial.print(runningCharge_mAh, 6);
		Serial.println(" mAh");

		Serial.print("[NVS] energy = ");
		Serial.print(runningEnergy_mWh, 6);
		Serial.println(" mWh");

		Serial.println("[NVS] Legacy checkpoint load: OK");

		return true;
	}

	// --------------------------------------------------------------------------
	// CURRENT SCHEMA 2
	// --------------------------------------------------------------------------

	if (schema == NVS_SCHEMA_VERSION)
	{
		if (!preferences.isKey("experiment") ||
				!preferences.isKey("interval") ||
				!preferences.isKey("charge") ||
				!preferences.isKey("energy"))
		{

			Serial.println(
					"[ERROR] Schema 2 checkpoint is incomplete.");

			preferences.end();
			return false;
		}

		experimentId =
				preferences.getUInt("experiment", 0);

		completedInterval =
				preferences.getUInt("interval", 0);

		runningCharge_mAh =
				preferences.getDouble("charge", 0.0);

		runningEnergy_mWh =
				preferences.getDouble("energy", 0.0);

		preferences.end();

		Serial.print("[NVS] experiment = ");
		Serial.println(experimentId);

		Serial.print("[NVS] interval = ");
		Serial.println(completedInterval);

		Serial.print("[NVS] charge = ");
		Serial.print(runningCharge_mAh, 6);
		Serial.println(" mAh");

		Serial.print("[NVS] energy = ");
		Serial.print(runningEnergy_mWh, 6);
		Serial.println(" mWh");

		Serial.println("[NVS] Namespace closed.");
		Serial.println("[NVS] Checkpoint load: OK");

		return true;
	}

	// Unknown schema means the firmware cannot safely interpret what is stored.
	Serial.print("[ERROR] Unsupported NVS schema: ");
	Serial.println(schema);

	preferences.end();

	return false;
}

// ============================================================================
// CSV OUTPUT
// ============================================================================
//
// CSV is PRINTED through Serial.
//
// It is not stored as a CSV file inside the ESP32.
//
// Each data row contains experiment_id.
//
// That makes this safe:
//
//   CSV_DATA,1,1,...
//   CSV_DATA,1,2,...
//   CSV_DATA,1,3,...
//
//   RESET YES
//
//   CSV_DATA,2,1,...
//   CSV_DATA,2,2,...
//
// Later we can simply select:
//
//   experiment_id == 2
//
// and know exactly which rows belong to that experiment.
// ============================================================================

void printCsvHeader()
{
	Serial.println();

	Serial.println(
			"[CSV] Machine-readable interval logging enabled.");

	Serial.println(
			"CSV_HEADER,"
			"experiment_id,"
			"interval,"
			"elapsed_seconds,"
			"voltage_V,"
			"current_mA,"
			"power_mW,"
			"temperature_C,"
			"interval_charge_mAh,"
			"interval_energy_mWh,"
			"average_current_mA,"
			"average_power_mW,"
			"running_charge_mAh,"
			"running_energy_mWh");
}

// Explicit machine-readable event showing that a NEW experiment started.
void printExperimentStartEvent()
{
	Serial.print("CSV_EVENT,EXPERIMENT_START,");
	Serial.println(experimentId);
}

// Explicit machine-readable event showing that an existing experiment was
// resumed after an ESP32 reboot.
void printExperimentResumeEvent()
{
	Serial.print("CSV_EVENT,EXPERIMENT_RESUME,");
	Serial.print(experimentId);
	Serial.print(',');
	Serial.println(completedInterval);
}

// Machine-readable markers for the deep-sleep power test.
//
// The host parser already treats everything after experiment_id as one
// comma-joined detail field, so these need no change on the Python side.
//
// Durability caveat, and it is a real one:
//
//   SLEEP_TEST_SLEEP is emitted at the end of an awake phase, while the host
//   is connected, so it is normally captured.
//
//   SLEEP_TEST_WAKE is emitted during setup(), before the host has finished
//   rediscovering the re-enumerated USB device, so it is normally LOST. This
//   is the same known limitation already documented for EXPERIMENT_RESUME and
//   is one of the reasons the store-and-forward backlog item exists.
void printSleepTestEvent(const char *eventType, uint32_t cycle)
{
	Serial.print("CSV_EVENT,");
	Serial.print(eventType);
	Serial.print(',');
	Serial.print(experimentId);
	Serial.print(',');
	Serial.println(cycle);
}

void printCsvRow(
		uint32_t interval,
		double elapsedSeconds,
		const SensorReading &reading,
		double intervalCharge_mAh,
		double intervalEnergy_mWh,
		double averageCurrent_mA,
		double averagePower_mW)
{

	Serial.print("CSV_DATA,");

	Serial.print(experimentId);

	Serial.print(',');
	Serial.print(interval);

	Serial.print(',');
	Serial.print(elapsedSeconds, 3);

	Serial.print(',');
	Serial.print(reading.voltage_V, 6);

	Serial.print(',');
	Serial.print(reading.current_mA, 6);

	Serial.print(',');
	Serial.print(reading.power_mW, 6);

	Serial.print(',');
	Serial.print(reading.temperature_C, 4);

	Serial.print(',');
	Serial.print(intervalCharge_mAh, 9);

	Serial.print(',');
	Serial.print(intervalEnergy_mWh, 9);

	Serial.print(',');
	Serial.print(averageCurrent_mA, 6);

	Serial.print(',');
	Serial.print(averagePower_mW, 6);

	Serial.print(',');
	Serial.print(runningCharge_mAh, 9);

	Serial.print(',');
	Serial.println(runningEnergy_mWh, 9);
}

// ============================================================================
// SERIAL COMMAND HELP
// ============================================================================

const char *wifiModeName(wifi_mode_t mode)
{
	switch (mode)
	{
	case WIFI_OFF:
		return "OFF";
	case WIFI_STA:
		return "STA";
	case WIFI_AP:
		return "AP";
	case WIFI_AP_STA:
		return "STA+AP";
	default:
		return "UNKNOWN";
	}
}

void printWifiStatus()
{
	wifi_mode_t mode = WiFi.getMode();

	Serial.print("[WIFI] State: ");
	Serial.println(mode == WIFI_OFF ? "DISABLED" : "ENABLED");

	Serial.print("[WIFI] Mode: ");
	Serial.println(wifiModeName(mode));
}

bool enableWifi()
{
	Serial.println("[WIFI] Enabling Wi-Fi...");

	// STA means station mode: the ESP32 behaves like a client radio, but this
	// command deliberately does not join an access point. Enabling the radio
	// without a network connection lets bench measurements isolate Wi-Fi power.
	if (!WiFi.mode(WIFI_STA))
	{
		Serial.println("[WIFI] ERROR: Could not set station mode.");
		return false;
	}

	if (!WiFi.disconnect(false, false))
	{
		Serial.println("[WIFI] ERROR: Could not keep Wi-Fi disconnected.");
		return false;
	}

	Serial.println("[WIFI] Mode: STA");
	Serial.println("[WIFI] Wi-Fi enabled: ALL OK");
	return true;
}

bool disableWifi()
{
	Serial.println("[WIFI] Disabling Wi-Fi...");

	// WIFI_OFF is the Arduino ESP32 core's explicit radio-off mode.
	if (!WiFi.mode(WIFI_OFF))
	{
		Serial.println("[WIFI] ERROR: Could not disable Wi-Fi.");
		return false;
	}

	Serial.println("[WIFI] Wi-Fi disabled: ALL OK");
	return true;
}

void initializeWifiOff()
{
	// Set the baseline explicitly instead of relying on framework defaults,
	// which can vary after resets or across Arduino core versions.
	Serial.println("[WIFI] Boot default: OFF");
	disableWifi();
}

void startWifiPowerTestPhase(uint8_t phase)
{
	wifiPowerTestPhase = phase;
	wifiPowerTestPhaseStartedMs = millis();

	if (phase == 1)
	{
		// Explicitly turn the radio OFF for each OFF phase, including adjacent
		// OFF phases at the boundary between repeated cycles.
		disableWifi();
		Serial.println("[POWER TEST] Phase 1/3: WIFI OFF for 30 seconds");
	}
	else if (phase == 2)
	{
		enableWifi();
		Serial.println("[POWER TEST] Phase 2/3: WIFI ON for 30 seconds");
	}
	else if (phase == 3)
	{
		disableWifi();
		Serial.println("[POWER TEST] Phase 3/3: WIFI OFF for 30 seconds");
	}
	else
	{
		Serial.print("[POWER TEST] ERROR: Invalid phase: ");
		Serial.println(phase);
	}
}

void updateWifiPowerTest(unsigned long now)
{
	if (!wifiPowerTestRunning ||
			now - wifiPowerTestPhaseStartedMs < WIFI_POWER_TEST_PHASE_MS)
	{
		return;
	}

	if (wifiPowerTestPhase == 1)
	{
		startWifiPowerTestPhase(2);
	}
	else if (wifiPowerTestPhase == 2)
	{
		startWifiPowerTestPhase(3);
	}
	else if (wifiPowerTestPhase == 3)
	{
		Serial.println("[POWER TEST] Cycle complete. Repeating...");
		startWifiPowerTestPhase(1);
	}
	else
	{
		Serial.println("[POWER TEST] ERROR: Stopping due to invalid phase.");
		wifiPowerTestRunning = false;
	}
}

bool armWifiPowerTest()
{
	if (autonomousTestArmed || autonomousTestRunning)
	{
		Serial.println(
				"[POWER TEST] ERROR: The autonomous logger bench test is armed.");
		Serial.println("[POWER TEST] Send LOGGER TEST STOP first.");
		return false;
	}

	// Only one power test may be armed at a time. Silently replacing an armed
	// test would change what the DMM is measuring without saying so.
	if (sleepPowerTestArmed || sleepPowerTestRunning)
	{
		Serial.println(
				"[POWER TEST] ERROR: The deep-sleep power test is already armed.");
		Serial.println(
				"[POWER TEST] Send POWER TEST STOP first, then POWER TEST WIFI.");
		return false;
	}

	if (!saveWifiPowerTestArmed(true))
	{
		Serial.println("[POWER TEST] ERROR: Test was not armed.");
		return false;
	}

	wifiPowerTestArmed = true;
	wifiPowerTestRunning = true;
	Serial.println("[POWER TEST] Wi-Fi power test ARMED");
	startWifiPowerTestPhase(1);
	return true;
}

// NOTE: The former stopWifiPowerTest() was removed when POWER TEST STOP began
// handling both power tests. Its logic now lives in stopAllPowerTests(), which
// is the single stop path. Two stop functions would eventually diverge.

void printWifiPowerTestStatus()
{
	Serial.print("[POWER TEST] Wi-Fi test armed: ");
	Serial.println(wifiPowerTestArmed ? "YES" : "NO");

	Serial.print("[POWER TEST] Wi-Fi test running: ");
	Serial.println(wifiPowerTestRunning ? "YES" : "NO");

	if (!wifiPowerTestRunning)
	{
		Serial.println("[POWER TEST] Wi-Fi test phase: NONE");
		return;
	}

	Serial.print("[POWER TEST] Wi-Fi test phase: ");
	Serial.print(wifiPowerTestPhase);
	Serial.println("/3");

	Serial.print("[POWER TEST] Elapsed in phase: ");
	Serial.print((millis() - wifiPowerTestPhaseStartedMs) / 1000UL);
	Serial.println(" seconds");
}

// ============================================================================
// WHICH POWER TEST IS ARMED
// ============================================================================

PowerTestMode armedPowerTestMode()
{
	// The autonomous bench test is not a power test, but it owns deep sleep and
	// the INA228 accumulators, so it is mutually exclusive with both of them.
	// Reporting it as a conflict here is what stops a power test from being
	// armed on top of it.
	if (autonomousTestArmed && (wifiPowerTestArmed || sleepPowerTestArmed))
	{
		return POWER_TEST_CONFLICT;
	}

	if (wifiPowerTestArmed && sleepPowerTestArmed)
	{
		return POWER_TEST_CONFLICT;
	}

	if (wifiPowerTestArmed)
	{
		return POWER_TEST_WIFI;
	}

	if (sleepPowerTestArmed)
	{
		return POWER_TEST_SLEEP;
	}

	return POWER_TEST_NONE;
}

// ============================================================================
// INTERVAL ACCOUNTING SUSPENSION
// ============================================================================
//
// The 60-second accounting interval cannot survive repeated deep sleep:
//
//   1. millis() restarts at zero on every wake, so the interval timer never
//      reaches 60 seconds during a 30-second awake phase.
//   2. setup() calls resetInaAccumulators() on every boot, so INA228 hardware
//      accumulation from the previous awake phase is discarded on each wake.
//   3. The board is not awake during sleep, so there is no honest way to
//      report 60 seconds of elapsed interval time.
//
// Rather than emit intervals whose elapsed time, average current, and average
// power would all be wrong, accounting is explicitly SUSPENDED for the whole
// duration of the deep-sleep test. Running charge, running energy, interval
// number, and experiment ID are left exactly as they were.
//
// This is a single derived predicate so no second flag can drift out of sync
// with the test state. It has two honest causes:
//
//   sleepPowerTestRunning     the deep-sleep test is in progress
//   intervalAccountingBlocked a test ended but the accumulators are dirty
//
// The second one exists because "the test stopped" and "accounting is safe to
// resume" are different facts. Without it, a failed accumulator reset would
// print that accounting stays suspended while this predicate said it was
// active, and loop() would go on closing intervals with stale accumulation.
// ============================================================================

// True only while the autonomous cycle actually owns the board.
//
// Deliberately NOT autonomousTestArmed, and no longer autonomousTestRunning:
// during a HOST SESSION the board is armed but awake and tethered, and normal
// interval accounting has to run. Suspending it there would silently stop the
// CSV telemetry the host connected to collect.
bool autonomousOwnsBoard()
{
	return autonomousState == AUTO_STATE_TIMER_WAKE_MEASUREMENT ||
				 autonomousState == AUTO_STATE_USB_RENDEZVOUS ||
				 autonomousState == AUTO_STATE_DEEP_SLEEP_PENDING;
}

bool intervalAccountingSuspended()
{
	return sleepPowerTestRunning || autonomousOwnsBoard() ||
				 intervalAccountingBlocked;
}

const char *intervalAccountingSuspendReason()
{
	if (autonomousOwnsBoard())
	{
		return "autonomous logger owns the board";
	}

	if (sleepPowerTestRunning)
	{
		return "deep-sleep power test in progress";
	}

	if (intervalAccountingBlocked)
	{
		return "INA228 accumulators could not be cleared; reset the board";
	}

	return "not suspended";
}

// ============================================================================
// DEEP-SLEEP WAKE DIAGNOSTICS
// ============================================================================
//
// Every value here comes from esp_sleep_get_wakeup_cause(). Nothing is
// inferred from timing, from the armed flag, or from RTC memory contents.
// ============================================================================

const char *wakeupCauseName(esp_sleep_wakeup_cause_t cause)
{
	switch (cause)
	{
	case ESP_SLEEP_WAKEUP_UNDEFINED:
		return "POWER ON / RESET";
	case ESP_SLEEP_WAKEUP_TIMER:
		return "DEEP SLEEP TIMER";
	case ESP_SLEEP_WAKEUP_EXT0:
		return "EXT0";
	case ESP_SLEEP_WAKEUP_EXT1:
		return "EXT1";
	case ESP_SLEEP_WAKEUP_TOUCHPAD:
		return "TOUCHPAD";
	case ESP_SLEEP_WAKEUP_ULP:
		return "ULP";
	case ESP_SLEEP_WAKEUP_GPIO:
		return "GPIO";
	case ESP_SLEEP_WAKEUP_UART:
		return "UART";
	default:
		return "UNRECOGNIZED";
	}
}

void printBootWakeReason()
{
	Serial.print("[BOOT] Wake reason: ");
	Serial.println(wakeupCauseName(bootWakeupCause));

	// Print the raw enum value too. If the name above ever says UNRECOGNIZED,
	// this is what makes the cause diagnosable instead of a dead end.
	Serial.print("[BOOT] Wake cause code: ");
	Serial.println(static_cast<int>(bootWakeupCause));
}

// ============================================================================
// DEEP-SLEEP POWER TEST: AWAKE PHASE
// ============================================================================

void startSleepPowerTestAwakePhase()
{
	sleepPowerTestRunning = true;
	sleepPowerTestAwakeStartedMs = millis();

	Serial.println();

	if (sleepPowerTestInaOff)
	{
		Serial.println("[POWER TEST] INA-OFF sleep test");
	}

	Serial.print("[POWER TEST] Sleep test variant: ");
	Serial.println(sleepPowerTestVariantName());

	Serial.println("[POWER TEST] Sleep test: AWAKE phase");

	Serial.print("[POWER TEST] Awake duration: ");
	Serial.print(SLEEP_POWER_TEST_AWAKE_MS / 1000UL);
	Serial.println(" seconds");

	// Report the Wi-Fi state the radio is ACTUALLY in rather than asserting
	// that it is off. If something left it on, this line has to say so.
	wifi_mode_t mode = WiFi.getMode();

	Serial.print("[POWER TEST] Wi-Fi: ");
	Serial.println(mode == WIFI_OFF ? "OFF" : wifiModeName(mode));

	if (mode != WIFI_OFF)
	{
		Serial.println(
				"[POWER TEST] ERROR: Wi-Fi must be OFF during the sleep test.");
		Serial.println(
				"[POWER TEST] Disabling Wi-Fi now so the measurement stays valid.");

		disableWifi();

		if (WiFi.getMode() != WIFI_OFF)
		{
			Serial.println(
					"[POWER TEST] ERROR: Wi-Fi is STILL ON. Sleep-current readings "
					"from this cycle include the radio.");
		}
	}

	Serial.print("[POWER TEST] Cycle: ");
	Serial.println(rtcSleepTestCycle);

	Serial.print("[POWER TEST] Boot-to-awake-phase time: ");
	Serial.print(millis() / 1000.0, 2);
	Serial.println(" seconds");

	Serial.println(
			"[POWER TEST] Interval accounting: SUSPENDED for this test.");
	Serial.println(
			"[POWER TEST] CSV_SAMPLE and CSV_DATA output: SUSPENDED for this test.");
	Serial.println(
			"[POWER TEST] Heartbeat continues every 5 seconds.");

	if (sleepPowerTestInaOff)
	{
		Serial.println(
				"[POWER TEST] During each deep sleep the INA228 will be in its own "
				"shutdown mode.");
		Serial.println(
				"[POWER TEST] Solar measurement is therefore SUSPENDED during sleep: "
				"no conversions and no CHARGE/ENERGY accumulation.");
		Serial.println(
				"[POWER TEST] The INA228 measures normally during this awake phase.");
	}
	else
	{
		Serial.println(
				"[POWER TEST] The INA228 stays in continuous conversion through the "
				"deep sleep.");
	}
}

// ============================================================================
// DEEP-SLEEP POWER TEST: ENTER SLEEP
// ============================================================================
//
// This function does not return in the success case. The chip powers down
// here and the next thing that runs is setup().
// ============================================================================

void enterSleepPowerTestDeepSleep()
{
	Serial.println();

	if (sleepPowerTestInaOff)
	{
		Serial.println("[POWER TEST] INA-OFF sleep test");

		// The INA228 is shut down BEFORE the wake timer is armed, so a shutdown
		// failure costs nothing: the board simply never sleeps this cycle.
		if (!enterInaShutdownMode())
		{
			Serial.println(
					"[POWER TEST] ERROR: INA228 shutdown could not be verified.");
			Serial.println("[POWER TEST] Deep sleep NOT entered.");
			Serial.println(
					"[POWER TEST] Sleeping now would measure an unknown INA228 state "
					"and the reading would be meaningless.");

			stopAllPowerTests();
			return;
		}

		inaShutdownActive = true;
	}

	Serial.print("[POWER TEST] Entering ESP32 DEEP SLEEP for ");
	Serial.print(static_cast<unsigned long>(SLEEP_POWER_TEST_SLEEP_US / 1000000ULL));
	Serial.println(" seconds...");

	// Reports the cycle whose awake phase just finished.
	printSleepTestEvent(
			sleepPowerTestInaOff ? "SLEEP_TEST_SLEEP_INA_OFF" : "SLEEP_TEST_SLEEP",
			rtcSleepTestCycle);

	esp_err_t timerResult =
			esp_sleep_enable_timer_wakeup(SLEEP_POWER_TEST_SLEEP_US);

	if (timerResult != ESP_OK)
	{
		// Sleeping without a wake source would put the board into a sleep it
		// never returns from. The DMM would show sleep current forever and the
		// test would look like it was working. Refuse, loudly, and disarm.
		Serial.print(
				"[POWER TEST] ERROR: Could not enable the deep-sleep wake timer. "
				"esp_err_t = ");
		Serial.println(static_cast<int>(timerResult));

		Serial.println("[POWER TEST] Deep sleep NOT entered.");
		Serial.println(
				"[POWER TEST] Stopping the sleep test rather than sleeping without "
				"a wake source.");

		stopAllPowerTests();
		return;
	}

	Serial.println("[POWER TEST] Wake timer armed: ALL OK");

	// Stamp RTC memory only now, once sleep is actually going to happen. If the
	// counter were advanced before the timer check above, a failed check would
	// leave a cycle count that claims a sleep the board never took.
	//
	// After the wake this stamp is the only evidence that the previous boot was
	// ours and ended deliberately.
	rtcSleepTestMagic = SLEEP_POWER_TEST_RTC_MAGIC;
	rtcSleepTestCycle++;

	// Let Serial finish transmitting before most of the chip powers down.
	Serial.flush();

	esp_deep_sleep_start();

	// esp_deep_sleep_start() is declared noreturn, so reaching this line means
	// something is deeply wrong with the platform. Say so rather than falling
	// through into a loop() that believes it is still in an awake phase.
	Serial.println(
			"[POWER TEST] ERROR: esp_deep_sleep_start() returned. Deep sleep did "
			"not happen.");
	Serial.println(
			"[POWER TEST] Stopping the sleep test; this cycle measured nothing.");

	stopAllPowerTests();
}

void updateSleepPowerTest(unsigned long now)
{
	if (!sleepPowerTestRunning)
	{
		return;
	}

	if (now - sleepPowerTestAwakeStartedMs < SLEEP_POWER_TEST_AWAKE_MS)
	{
		return;
	}

	enterSleepPowerTestDeepSleep();
}

bool armSleepPowerTest(bool inaOff)
{
	if (autonomousTestArmed || autonomousTestRunning)
	{
		Serial.println(
				"[POWER TEST] ERROR: The autonomous logger bench test is armed.");
		Serial.println("[POWER TEST] Send LOGGER TEST STOP first.");
		return false;
	}

	if (wifiPowerTestArmed || wifiPowerTestRunning)
	{
		Serial.println(
				"[POWER TEST] ERROR: The Wi-Fi power test is already armed.");
		Serial.println(
				"[POWER TEST] Send POWER TEST STOP first, then arm the sleep test.");
		return false;
	}

	if (sleepPowerTestArmed)
	{
		if (sleepPowerTestInaOff == inaOff)
		{
			// Idempotent success: this exact variant is already armed.
			Serial.print(
					"[POWER TEST] Deep-sleep power test is already armed in this "
					"variant: ");
			Serial.println(sleepPowerTestVariantName());
			Serial.println("[POWER TEST] No change made.");
			return true;
		}

		// Switching variants silently would change what the DMM is measuring
		// without saying so, which is exactly the failure this refuses.
		Serial.print(
				"[POWER TEST] ERROR: The deep-sleep test is already armed in the "
				"other variant: ");
		Serial.println(sleepPowerTestVariantName());
		Serial.println(
				"[POWER TEST] Send POWER TEST STOP first, then arm the variant you "
				"want.");
		return false;
	}

	// Write the variant BEFORE the armed flag. If the variant write fails the
	// test is not armed at all, so a reboot can never resume a test whose
	// variant is unknown.
	if (!saveSleepPowerTestInaOff(inaOff))
	{
		Serial.println(
				"[POWER TEST] ERROR: Deep-sleep variant was NOT saved, so the test "
				"was not armed.");
		return false;
	}

	sleepPowerTestInaOff = inaOff;

	if (!saveSleepPowerTestArmed(true))
	{
		Serial.println(
				"[POWER TEST] ERROR: Deep-sleep test was NOT armed. It would not "
				"survive the USB disconnect, so it is not being started either.");
		return false;
	}

	sleepPowerTestArmed = true;

	// A freshly armed test starts its cycle count at zero.
	rtcSleepTestMagic = SLEEP_POWER_TEST_RTC_MAGIC;
	rtcSleepTestCycle = 0;

	Serial.println();
	Serial.print("[POWER TEST] Deep-sleep power test ARMED, variant: ");
	Serial.println(sleepPowerTestVariantName());
	Serial.println(
			"[POWER TEST] The armed flag and the variant are persisted in NVS, so "
			"this test survives USB disconnect and external power cycling.");
	Serial.println(
			"[POWER TEST] INA228 stays electrically powered from XIAO 3V3 in both "
			"variants. Nothing power-gates the INA228.");

	if (inaOff)
	{
		Serial.println(
				"[POWER TEST] The INA228 ADC will be placed in its documented "
				"shutdown mode before each deep sleep.");
		Serial.println(
				"[POWER TEST] Solar measurement is SACRIFICED during each sleep "
				"phase. That is the purpose of this variant.");
	}

	// Wi-Fi must be off for the whole test, including this first awake phase.
	disableWifi();

	printSleepTestEvent(
			inaOff ? "SLEEP_TEST_ARMED_INA_OFF" : "SLEEP_TEST_ARMED",
			rtcSleepTestCycle);

	startSleepPowerTestAwakePhase();
	return true;
}

// ============================================================================
// RESUME NORMAL INTERVAL ACCOUNTING
// ============================================================================
//
// Called when a deep-sleep test ends. The INA228 has been accumulating charge
// through a window we deliberately did not account for, and that window is
// not 60 seconds long. Folding it into the next interval would corrupt that
// interval's average current and average power.
//
// So the accumulators are cleared and the interval timer restarts now. The
// charge measured during the suspended window is discarded on purpose; the
// running totals from before the test are untouched.
// ============================================================================

bool resumeIntervalAccounting()
{
	Serial.println(
			"[POWER TEST] Resuming normal 60-second interval accounting.");

	if (!resetInaAccumulators())
	{
		// Fail closed. Resuming with stale accumulation would produce a wrong
		// interval that looks exactly like a right one.
		intervalAccountingBlocked = true;

		Serial.println(
				"[ERROR] Could not clear INA228 accumulators after the power test.");
		Serial.println(
				"[ERROR] Interval accounting stays SUSPENDED so a partial "
				"accumulation window cannot be reported as a 60-second interval.");
		Serial.println(
				"[ERROR] Running charge and running energy are preserved unchanged.");
		Serial.println(
				"[ERROR] Reset the board once Serial access is available.");
		return false;
	}

	intervalAccountingBlocked = false;

	intervalStartMs = millis();
	lastHeartbeatMs = intervalStartMs;
	lastLiveSampleMs = intervalStartMs;

	Serial.print("[POWER TEST] Experiment ");
	Serial.print(experimentId);
	Serial.print(" continues at interval ");
	Serial.print(completedInterval);
	Serial.println(" with totals unchanged.");

	Serial.println("[POWER TEST] Interval accounting: ACTIVE");
	return true;
}

// ============================================================================
// STOP WHICHEVER POWER TEST IS ACTIVE
// ============================================================================

bool stopAllPowerTests()
{
	// REFUSE while the autonomous cycle owns the board.
	//
	// The rendezvous and the cold-boot window both pump the command parser, so
	// this function is reachable mid-wake. It used to run to completion there and
	// call resumeIntervalAccounting(), which resets the INA228 accumulators - the
	// ones the wake cycle had just reset to open a fresh autonomous interval,
	// while rtcAutoIntervalStartMs was left untouched. The next record then
	// reported a full cadence in interval_ms against charge accumulated only
	// since the command: under-counted, CRC-valid, and unflagged.
	//
	// A power test and autonomous mode are already mutually exclusive at arming
	// time, so there is nothing legitimate to stop here. RESET YES takes the same
	// shape for the same reason.
	//
	// The cold-boot maintenance window is deliberately NOT covered by
	// autonomousOwnsBoard(), so the recovery path for a board that somehow holds
	// both flags is still reset-then-stop inside that window.
	if (autonomousOwnsBoard())
	{
		Serial.println();
		Serial.println(
				"[POWER TEST] ERROR: The autonomous cycle currently owns the board.");
		Serial.print("[POWER TEST] State: ");
		Serial.println(autoStateName(autonomousState));
		Serial.println(
				"[POWER TEST] Stopping a power test here would reset the INA228 "
				"accumulators in the middle of an autonomous interval and silently "
				"under-count it. No change was made.");
		Serial.println(
				"[POWER TEST] Reset the board and send POWER TEST STOP inside the "
				"cold-boot maintenance window instead.");
		return false;
	}

	bool wifiTestWasActive = wifiPowerTestArmed || wifiPowerTestRunning;
	bool sleepTestWasActive = sleepPowerTestArmed || sleepPowerTestRunning;

	// Only the suspensions this function is actually clearing may be resumed by
	// it. intervalAccountingSuspended() also answers true for autonomous
	// ownership, which this function does not own and must not resume - that was
	// the second half of the same defect, and it fired even when no power test
	// was armed at all.
	bool accountingWasSuspended =
			sleepPowerTestRunning || intervalAccountingBlocked;

	Serial.println();

	if (!wifiTestWasActive && !sleepTestWasActive)
	{
		Serial.println("[POWER TEST] No power test was armed or running.");

		if (!accountingWasSuspended)
		{
			Serial.println(
					"[POWER TEST] Nothing to stop. The INA228 accumulators and the "
					"interval timer are left alone.");
		}
	}

	// Clear RAM state first so nothing else acts on a half-stopped test.
	wifiPowerTestRunning = false;
	wifiPowerTestPhase = 0;
	sleepPowerTestRunning = false;

	// Wi-Fi must end OFF no matter which test was running.
	disableWifi();

	// The INA228 must end in normal continuous conversion no matter which test
	// was running. This matters on the INA-OFF failure path, where shutdown
	// succeeded but the ESP32 then did not sleep: without this, the logger
	// would resume with a device that is not converting.
	bool inaRestored = restoreInaContinuousMode();

	if (inaRestored)
	{
		inaShutdownActive = false;
	}
	else
	{
		Serial.println(
				"[POWER TEST] ERROR: INA228 is not confirmed to be measuring. "
				"Readings must not be trusted until this is resolved.");
	}

	if (WiFi.getMode() != WIFI_OFF)
	{
		Serial.println(
				"[POWER TEST] ERROR: Wi-Fi did not turn off. Radio state is unknown.");
	}

	bool persistedStateCleared = true;

	if (wifiPowerTestArmed)
	{
		if (saveWifiPowerTestArmed(false))
		{
			wifiPowerTestArmed = false;
			Serial.println("[POWER TEST] Wi-Fi test flag cleared.");
		}
		else
		{
			Serial.println(
					"[POWER TEST] ERROR: Could not clear the persisted Wi-Fi test flag. "
					"It will still be armed after the next reboot.");
			persistedStateCleared = false;
		}
	}

	if (!inaRestored)
	{
		persistedStateCleared = false;
	}

	if (sleepPowerTestArmed)
	{
		Serial.print("[POWER TEST] Stopping deep-sleep test variant: ");
		Serial.println(sleepPowerTestVariantName());

		if (saveSleepPowerTestArmed(false))
		{
			sleepPowerTestArmed = false;
			Serial.println("[POWER TEST] Deep-sleep test flag cleared.");
		}
		else
		{
			Serial.println(
					"[POWER TEST] ERROR: Could not clear the persisted deep-sleep test "
					"flag. It will still be armed after the next reboot.");
			persistedStateCleared = false;
		}

		// Clear the variant selector too, so a later bare POWER TEST SLEEP can
		// never inherit INA-OFF behavior from a test that was already stopped.
		if (sleepPowerTestInaOff)
		{
			if (saveSleepPowerTestInaOff(false))
			{
				sleepPowerTestInaOff = false;
				Serial.println("[POWER TEST] Deep-sleep variant flag cleared.");
			}
			else
			{
				Serial.println(
						"[POWER TEST] ERROR: Could not clear the persisted deep-sleep "
						"variant flag.");
				persistedStateCleared = false;
			}
		}
	}

	if (sleepTestWasActive)
	{
		printSleepTestEvent("SLEEP_TEST_STOPPED", rtcSleepTestCycle);

		Serial.print("[POWER TEST] Completed sleep cycles this power-on: ");
		Serial.println(rtcSleepTestCycle);

		// The counter only describes one continuous battery run, so clear it
		// along with the test rather than letting it leak into the next one.
		rtcSleepTestMagic = 0;
		rtcSleepTestCycle = 0;
	}

	if (accountingWasSuspended)
	{
		if (!resumeIntervalAccounting())
		{
			persistedStateCleared = false;
		}
	}

	if (persistedStateCleared)
	{
		Serial.println("[POWER TEST] Power test stopped: ALL OK");
		Serial.println("[POWER TEST] Normal continuous logging resumed.");
	}
	else
	{
		Serial.println(
				"[POWER TEST] NOT ALL OK - power test stop did not fully succeed.");
	}

	return persistedStateCleared;
}

// ============================================================================
// COMBINED POWER-TEST STATUS
// ============================================================================

void printPowerTestStatus()
{
	Serial.println();

	Serial.print("[POWER TEST] Active test: ");

	switch (armedPowerTestMode())
	{
	case POWER_TEST_NONE:
		Serial.println("NONE");
		break;
	case POWER_TEST_WIFI:
		Serial.println("WIFI");
		break;
	case POWER_TEST_SLEEP:
		// Naming the variant here is the whole point: "SLEEP" alone would not
		// tell you whether the INA228 is converting through the sleep phase.
		Serial.print("SLEEP (");
		Serial.print(sleepPowerTestVariantName());
		Serial.println(")");
		break;
	case POWER_TEST_CONFLICT:
		Serial.println("CONFLICT");
		Serial.println(
				"[POWER TEST] ERROR: Both power-test flags are armed. No test is "
				"running. Send POWER TEST STOP to clear both.");
		break;
	}

	printWifiPowerTestStatus();

	Serial.print("[POWER TEST] Sleep test armed: ");
	Serial.println(sleepPowerTestArmed ? "YES" : "NO");

	if (sleepPowerTestArmed)
	{
		Serial.print("[POWER TEST] Sleep test variant: ");
		Serial.println(sleepPowerTestVariantName());

		Serial.print("[POWER TEST] INA228 during sleep phase: ");
		Serial.println(
				sleepPowerTestInaOff
						? "SHUTDOWN, measurement suspended"
						: "continuous conversion, measurement active");
	}

	Serial.print("[POWER TEST] Sleep test running: ");
	Serial.println(sleepPowerTestRunning ? "YES" : "NO");

	// Report what the INA228 says about itself, not what we intended.
	Serial.print("[POWER TEST] INA228 mode right now: ");

	if (inaIsInContinuousMode(false))
	{
		Serial.println("continuous conversion");
	}
	else
	{
		Serial.println("NOT continuous conversion");
		printPowerTestInaModeDetail();
	}

	if (sleepPowerTestRunning)
	{
		Serial.print("[POWER TEST] Sleep test cycle: ");
		Serial.println(rtcSleepTestCycle);

		Serial.print("[POWER TEST] Elapsed in awake phase: ");
		Serial.print((millis() - sleepPowerTestAwakeStartedMs) / 1000.0, 1);
		Serial.println(" seconds");

		Serial.print("[POWER TEST] Awake phase length: ");
		Serial.print(SLEEP_POWER_TEST_AWAKE_MS / 1000UL);
		Serial.println(" seconds");

		Serial.print("[POWER TEST] Deep sleep length: ");
		Serial.print(
				static_cast<unsigned long>(SLEEP_POWER_TEST_SLEEP_US / 1000000ULL));
		Serial.println(" seconds");
	}

	Serial.print("[POWER TEST] Interval accounting: ");

	if (intervalAccountingSuspended())
	{
		Serial.print("SUSPENDED: ");
		Serial.println(intervalAccountingSuspendReason());
	}
	else
	{
		Serial.println("ACTIVE");
	}

	printBootWakeReason();
}

void printHelp()
{
	Serial.println();

	Serial.println("[COMMAND] Available commands:");

	Serial.println(
			"  HELP       - show this list");

	Serial.println(
			"  STATUS     - print current experiment state and live sensor reading");

	Serial.println(
			"  VERSION    - print firmware version, source revision, and build ID");

	Serial.println(
			"  WIFI ON    - enable Wi-Fi station mode without connecting");

	Serial.println(
			"  WIFI OFF   - disable the Wi-Fi subsystem");

	Serial.println(
			"  WIFI STATUS - print Wi-Fi state and mode");

	Serial.println(
			"  POWER TEST WIFI   - arm the repeating Wi-Fi power test");

	Serial.println(
			"  POWER TEST SLEEP  - arm the deep-sleep power test, INA228 stays in "
			"continuous conversion");

	Serial.println(
			"  POWER TEST SLEEP INA OFF - arm the deep-sleep power test, INA228 "
			"enters its own shutdown mode before each sleep");

	Serial.println(
			"  POWER TEST STOP   - stop whichever power test is active");

	Serial.println(
			"  POWER TEST STATUS - print power-test state");

	Serial.println(
			"  LOGGER AUTONOMOUS ON     - enable autonomous sleep/wake/store mode");

	Serial.println(
			"  LOGGER AUTONOMOUS OFF    - disable it (only parsed while awake: "
			"during a USB rendezvous, a host session, or the cold-boot maintenance "
			"window)");

	Serial.println(
			"  LOGGER AUTONOMOUS STATUS - mode, cadence, storage, session state");

	Serial.println(
			"  LOGGER TEST AUTONOMOUS / STOP / STATUS - DEPRECATED aliases for the "
			"three above");

	Serial.println(
			"  LOGGER SESSION HOLD - claim this wake; stay awake while a host is "
			"attached (autonomous mode stays ARMED)");

	Serial.println(
			"  LOGGER SESSION KEEPALIVE - renew the host lease");

	Serial.println(
			"  LOGGER SESSION RELEASE - hand the board back to autonomous sleep "
			"(does NOT disarm autonomous mode)");

	Serial.println(
			"  LOGGER SESSION STATUS - print state, lease, and rendezvous info");

	Serial.println(
			"  LOGGER INTERVAL <seconds> - set the autonomous cadence");

	Serial.println(
			"  LOGGER STORAGE INFO - filesystem and durable-log summary");

	Serial.println(
			"  LOGGER STORAGE DUMP - decode stored records to Serial");

	Serial.println(
			"  LOGGER STORAGE CLEAR YES - erase stored records (NOT experiment state)");

	Serial.println(
			"  RESET      - explain reset confirmation");

	Serial.println(
			"  RESET YES  - start a NEW experiment");

	Serial.println();
}

// ============================================================================
// FIRMWARE IDENTITY
// ============================================================================
//
// Printed at boot, inside STATUS, and on demand via the VERSION command, so
// "what is actually on this board?" can be answered without a reset and
// without a full STATUS block.
//
// The three lines are always printed together. A revision without a version
// says nothing about intent, and a version without a revision cannot be tied
// back to source.

void printFirmwareIdentity()
{
	Serial.print("[FIRMWARE] Version:  ");
	Serial.println(FIRMWARE_VERSION);

	Serial.print("[FIRMWARE] Revision: ");

	if (FIRMWARE_GIT_REV[0] == '\0')
	{
		// Never a blank, never a plausible-looking placeholder. An image whose
		// source cannot be identified has to say that it cannot be identified.
		Serial.println(
				"UNKNOWN - not injected by this build (use tools/upload.sh)");
	}
	else
	{
		Serial.println(FIRMWARE_GIT_REV);
	}

	Serial.print("[FIRMWARE] Build ID: ");
	Serial.println(FIRMWARE_BUILD_ID);
}

// ============================================================================
// STATUS COMMAND
// ============================================================================

bool printStatus()
{
	Serial.println();

	Serial.println("[STATUS] Current logger state:");
	printFirmwareIdentity();

	Serial.print("[STATUS] Experiment ID      = ");
	Serial.println(experimentId);

	Serial.print("[STATUS] Completed interval = ");
	Serial.println(completedInterval);

	Serial.print("[STATUS] Running charge     = ");
	Serial.print(runningCharge_mAh, 6);
	Serial.println(" mAh");

	Serial.print("[STATUS] Running energy     = ");
	Serial.print(runningEnergy_mWh, 6);
	Serial.println(" mWh");

	// Without this line, the interval number above reads like a live counter
	// while it is actually frozen.
	if (intervalAccountingSuspended())
	{
		Serial.print("[STATUS] Interval accounting = SUSPENDED: ");
		Serial.println(intervalAccountingSuspendReason());
		Serial.println(
				"[STATUS] The interval and totals above are frozen, not advancing.");
	}
	else
	{
		Serial.println("[STATUS] Interval accounting = ACTIVE");
	}

	if (inaShutdownActive)
	{
		// Not a failure. The INA-OFF power-test variant shuts the part down on
		// purpose, and refusing to print stale registers is the correct behavior
		// (D-016), so the command did exactly what it should.
		Serial.println(
				"[STATUS] INA228               = SHUTDOWN, measurement suspended");
		Serial.println(
				"[STATUS] Voltage/current/power are NOT being updated and are not "
				"reported.");
		return true;
	}

	SensorReading reading;

	if (readSensor(reading, false))
	{
		Serial.print("[STATUS] Voltage            = ");
		Serial.print(reading.voltage_V, 4);
		Serial.println(" V");

		Serial.print("[STATUS] Current            = ");
		Serial.print(reading.current_mA, 3);
		Serial.println(" mA");

		Serial.print("[STATUS] Power              = ");
		Serial.print(reading.power_mW, 3);
		Serial.println(" mW");
	}
	else
	{
		Serial.println(
				"[ERROR] STATUS could not read INA228.");
		return false;
	}

	return true;
}

// ============================================================================
// START A NEW EXPERIMENT
// ============================================================================

bool resetExperiment()
{
	// A new experiment created while accounting is suspended would be frozen
	// from the moment it was born, and its first interval would never close.
	// Refuse instead of creating an experiment that silently does nothing.
	if (intervalAccountingSuspended())
	{
		Serial.println();
		Serial.print("[RESET] ERROR: Interval accounting is SUSPENDED: ");
		Serial.println(intervalAccountingSuspendReason());
		Serial.println(
				"[RESET] A new experiment would be created and then immediately "
				"frozen. Its first interval would never close.");
		Serial.println(
				"[RESET] Clear the suspension first, then send RESET YES again.");
		Serial.println("[RESET] No experiment was created.");
		return false;
	}

	intervalStartMs = millis();
	lastHeartbeatMs = intervalStartMs;
	lastLiveSampleMs = intervalStartMs;

	Serial.println();

	Serial.println(
			"============================================================");

	Serial.println("[RESET] RESET YES received.");

	Serial.println(
			"[RESET] Starting a NEW experiment.");

	if (experimentId == UINT32_MAX)
	{
		Serial.println(
				"[FATAL] experiment_id overflow. Cannot safely create another ID.");

		return false;
	}

	// First clear the INA228's physical hardware accumulators.
	//
	// We do this before changing our software totals so an INA failure cannot
	// silently give us a supposedly-clean experiment that actually contains old
	// hardware accumulation.
	if (!resetInaAccumulators())
	{
		Serial.println(
				"[ERROR] Experiment reset aborted because INA228 reset failed.");

		Serial.println(
				"============================================================");

		return false;
	}

	// Now establish the new software experiment.
	experimentId++;

	completedInterval = 0;
	runningCharge_mAh = 0.0;
	runningEnergy_mWh = 0.0;

	// Immediately save the new experiment ID and zero totals.
	//
	// This means a power failure one second later still boots into the NEW
	// experiment rather than bringing the previous experiment back.
	if (!saveCheckpoint())
	{
		Serial.println(
				"[ERROR] New experiment exists in RAM, but its zero checkpoint "
				"could not be saved.");

		Serial.println(
				"[ERROR] Do not trust power-loss recovery until NVS succeeds.");

		return false;
	}

	// Start timing interval #1 NOW.
	intervalStartMs = millis();
	lastHeartbeatMs = intervalStartMs;
	lastLiveSampleMs = intervalStartMs;

	Serial.print("[RESET] experiment_id = ");
	Serial.println(experimentId);

	Serial.println("[RESET] interval = 0");

	Serial.println(
			"[RESET] running charge = 0.000000 mAh");

	Serial.println(
			"[RESET] running energy = 0.000000 mWh");

	Serial.println(
			"[RESET] New 60-second interval started NOW.");

	Serial.println(
			"[RESET] RESET COMPLETE: ALL OK");

	Serial.println(
			"============================================================");

	// Machine-readable marker.
	printExperimentStartEvent();

	printCsvHeader();

	return true;
}

// ============================================================================
// PROCESS ONE COMPLETE SERIAL COMMAND
// ============================================================================

bool writeProtocolLine(const String &line, uint32_t timeoutMs)
{
	String framed = line;
	framed += '\n';

	const uint32_t deadline = millis() + timeoutMs;
	size_t offset = 0;

	while (offset < framed.length())
	{
		const size_t written = Serial.write(
				reinterpret_cast<const uint8_t *>(framed.c_str()) + offset,
				framed.length() - offset);
		offset += written;

		if (offset == framed.length())
		{
			return true;
		}

		if ((int32_t)(deadline - millis()) <= 0)
		{
			return false;
		}

		delay(1);
	}

	return true;
}

bool emitCommandAck(const String &command)
{
	String line = "CMD_ACK,";
	line += command;
	return writeProtocolLine(line, COMMAND_PROTOCOL_WRITE_TIMEOUT_MS);
}

bool emitCommandResult(const String &command, const char *status)
{
	String line = "CMD_RESULT,";
	line += command;
	line += ',';
	line += status;
	return writeProtocolLine(line, COMMAND_PROTOCOL_WRITE_TIMEOUT_MS);
}

void processCommand(String command)
{
	// Remove leading/trailing whitespace.
	command.trim();

	// Make command matching case-insensitive.
	//
	// status
	// STATUS
	// Status
	//
	// all become:
	//
	// STATUS
	command.toUpperCase();

	if (command.length() == 0)
	{
		return;
	}

	// CMD_ACK means PARSED AND ACCEPTED FOR EXECUTION, and nothing more. It is
	// emitted before dispatch, so it is deliberately still emitted for a command
	// that is about to be refused (D-030).
	if (!emitCommandAck(command))
	{
		// The bounded writer missed its 100 ms deadline, so the host will not see
		// an ACK and will treat this command as unheard even though it is about to
		// run. Saying so locally is the only place this is observable.
		Serial.println(
				"[COMMAND] ERROR: CMD_ACK was not fully written within the protocol "
				"deadline. The host cannot see that this command was parsed.");
	}

	Serial.print("[COMMAND] Received: ");
	Serial.println(command);

	// --------------------------------------------------------------------------
	// COMMAND RESULT CONTRACT
	// --------------------------------------------------------------------------
	//
	//   CMD_ACK,<command>            parsed and accepted for execution
	//   CMD_RESULT,<command>,OK      the command actually COMPLETED
	//   CMD_RESULT,<command>,ERROR   parsed, but did not complete successfully
	//
	// `commandOk` used to be `recognized`, which answered only "did the dispatcher
	// match a branch". That reported OK for a LOGGER AUTONOMOUS OFF whose NVS
	// write failed, for a LOGGER INTERVAL that was rejected as out of range, for a
	// RESET YES that was refused, and for a POWER TEST STOP that could not clear a
	// persisted flag. tools/send.sh printed ALL OK and exited 0 for every one of
	// them - the same class of defect as the 2026-09-11 failure that produced
	// D-022, reproduced inside the machine protocol built to prevent it.
	//
	// Every handler with a meaningful failure mode now returns bool and that
	// return lands here. Handlers that cannot fail stay void and leave commandOk
	// true. Refusals count as failures: a command that was declined did not do
	// what was asked.
	//
	// Two deliberate exceptions, both idempotent successes rather than no-ops:
	// arming something already armed, and disarming something already off. The
	// requested end state holds and nothing failed.
	bool resultEmitted = false;
	bool recognized = true;
	bool commandOk = true;

	if (command == "HELP")
	{

		printHelp();
	}
	else if (command == "STATUS")
	{

		commandOk = printStatus();
	}
	else if (command == "VERSION")
	{

		// Separate from STATUS on purpose. "Which image is on the board?" is
		// asked far more often than the full state block, and it has to be
		// answerable inside a short rendezvous window.
		printFirmwareIdentity();
	}
	else if (command == "WIFI ON")
	{

		commandOk = enableWifi();
	}
	else if (command == "WIFI OFF")
	{

		commandOk = disableWifi();
	}
	else if (command == "WIFI STATUS")
	{

		printWifiStatus();
	}
	else if (command == "POWER TEST WIFI")
	{

		commandOk = armWifiPowerTest();
	}
	else if (command == "POWER TEST SLEEP")
	{

		commandOk = armSleepPowerTest(false);
	}
	else if (command == "POWER TEST SLEEP INA OFF")
	{

		commandOk = armSleepPowerTest(true);
	}
	else if (command == "POWER TEST STOP")
	{

		commandOk = stopAllPowerTests();
	}
	else if (command == "POWER TEST STATUS")
	{

		printPowerTestStatus();
	}
	else if (command == "LOGGER AUTONOMOUS ON")
	{

		commandOk = armAutonomousTest();
	}
	else if (command == "LOGGER AUTONOMOUS OFF")
	{

		commandOk = stopAutonomousTest();
	}
	else if (command == "LOGGER AUTONOMOUS STATUS")
	{

		printAutonomousStatus();
	}
	else if (command == "LOGGER TEST AUTONOMOUS")
	{

		printDeprecatedCommand("LOGGER TEST AUTONOMOUS", "LOGGER AUTONOMOUS ON");
		commandOk = armAutonomousTest();
	}
	else if (command == "LOGGER TEST STATUS")
	{

		printDeprecatedCommand("LOGGER TEST STATUS", "LOGGER AUTONOMOUS STATUS");
		printAutonomousStatus();
	}
	else if (command == "LOGGER TEST STOP")
	{

		printDeprecatedCommand("LOGGER TEST STOP", "LOGGER AUTONOMOUS OFF");
		commandOk = stopAutonomousTest();
	}
	else if (command == "LOGGER SESSION HOLD")
	{

		// REFUSED: autonomous mode is off, so no lease is needed. ERROR: the board
		// is already committed to a deferred autonomous sleep (D-044).
		const HoldResult hold = hostSessionHold();
		emitCommandResult(
				command,
				hold == HOLD_GRANTED
						? "OK"
						: (hold == HOLD_NOT_ARMED ? "REFUSED" : "ERROR"));
		resultEmitted = true;
	}
	else if (command == "LOGGER SESSION KEEPALIVE")
	{

		const uint32_t keepalivesBefore = hostKeepaliveCount;
		hostSessionKeepalive();
		emitCommandResult(
				command,
				hostKeepaliveCount > keepalivesBefore ? "OK" : "FAILED");
		resultEmitted = true;
	}
	else if (command == "LOGGER SESSION RELEASE")
	{

		// OK only when the release COMPLETED (D-044): the session ended and, with
		// autonomous mode armed, the handoff was prepared and the deferred sleep
		// armed. It used to be keyed on wasHeld alone, so a handoff that failed and
		// left the board awake still answered OK.
		Serial.println("[SESSION] RELEASE parsed");
		const bool wasHeld = hostSessionHeld;
		const bool released = hostSessionRelease();
		const bool resultQueued = emitCommandResult(
				command, wasHeld ? (released ? "OK" : "ERROR") : "NOT_HELD");
		Serial.println(
				resultQueued ? "[SESSION] RELEASE result queued"
										 : "[SESSION] ERROR: RELEASE result not fully queued");
		resultEmitted = true;
	}
	else if (command == "LOGGER SESSION STATUS")
	{

		printHostSessionStatus();
	}
	else if (command == "LOGGER STORAGE INFO")
	{

		commandOk = printStorageInfo();
	}
	else if (command == "LOGGER STORAGE DUMP")
	{

		commandOk = dumpStorage();
	}
	else if (command == "LOGGER STORAGE CLEAR")
	{

		Serial.println(
				"[STORAGE] Clearing storage requires confirmation.");
		Serial.println(
				"[STORAGE] Type exactly: LOGGER STORAGE CLEAR YES");
	}
	else if (command == "LOGGER STORAGE CLEAR YES")
	{

		commandOk = clearStorage();
	}
	else if (command.startsWith("LOGGER INTERVAL "))
	{

		// The one authoritative cadence setting. Everything in the autonomous
		// path derives from it, so this is the only place it changes.
		String argument = command.substring(16);
		argument.trim();

		long seconds = argument.toInt();

		if (seconds <= 0)
		{
			Serial.print("[AUTO] ERROR: Not a valid interval: ");
			Serial.println(argument);
			commandOk = false;
		}
		else if (seconds < static_cast<long>(AUTO_INTERVAL_SECONDS_MIN) ||
						 seconds > static_cast<long>(AUTO_INTERVAL_SECONDS_MAX))
		{

			Serial.print("[AUTO] ERROR: Interval must be between ");
			Serial.print(AUTO_INTERVAL_SECONDS_MIN);
			Serial.print(" and ");
			Serial.print(AUTO_INTERVAL_SECONDS_MAX);
			Serial.print(" seconds. Got ");
			Serial.println(seconds);
			commandOk = false;
		}
		else if (autonomousTestArmed || autonomousTestRunning)
		{
			Serial.println(
					"[AUTO] ERROR: Cannot change the interval while the test is armed.");
			Serial.println(
					"[AUTO] Send LOGGER TEST STOP first, so stored records are not "
					"split across two cadences without an explicit boundary.");
			commandOk = false;
		}
		else if (saveAutonomousInterval(static_cast<uint32_t>(seconds)))
		{
			autonomousIntervalSeconds = static_cast<uint32_t>(seconds);
			Serial.print("[AUTO] Autonomous interval set to ");
			Serial.print(autonomousIntervalSeconds);
			Serial.println(" seconds: ALL OK");
		}
		else
		{
			Serial.println("[AUTO] ERROR: Interval was not saved.");
			commandOk = false;
		}
	}
	else if (command == "RESET")
	{

		Serial.println(
				"[COMMAND] RESET requires confirmation.");

		Serial.println(
				"[COMMAND] Type exactly: RESET YES");
	}
	else if (command == "RESET YES")
	{

		commandOk = resetExperiment();
	}
	else
	{
		recognized = false;

		Serial.print("[WARNING] Unknown command: ");
		Serial.println(command);

		Serial.println(
				"[WARNING] Type HELP for available commands.");
	}

	if (!resultEmitted)
	{
		// An unrecognized command never ran, so it cannot have completed. A
		// recognized one reports what its handler actually did.
		const bool succeeded = recognized && commandOk;

		if (!emitCommandResult(command, succeeded ? "OK" : "ERROR"))
		{
			Serial.println(
					"[COMMAND] ERROR: CMD_RESULT was not fully written within the "
					"protocol deadline. The host cannot see how this command ended.");
		}

		// The machine line is the protocol. This one is for a person reading the
		// console, and it exists so a failure is visible in the same place the
		// handler's own ERROR lines are.
		if (!succeeded)
		{
			Serial.print("[COMMAND] NOT ALL OK - ");
			Serial.print(command);
			Serial.println(recognized ? " did not complete successfully."
																: " is not a known command.");
		}
	}
}

// ============================================================================
// RECEIVE SERIAL COMMAND CHARACTERS
// ============================================================================
//
// There are TWO buffers involved.
//
// 1. Arduino/ESP32 Serial receive buffer
//
//    Managed for us by the framework.
//
//    Serial.available() tells us how many unread bytes are waiting there.
//
//
// 2. serialCommandBuffer
//
//    Managed by OUR firmware.
//
//    We move characters from the framework's receive buffer into our String
//    until Enter tells us that the command is complete.
// ============================================================================

void handleSerialCommands()
{
	// Several characters may have arrived since loop() last ran, so read all
	// currently waiting characters.
	while (Serial.available())
	{

		// Serial.read() removes ONE byte from the incoming receive buffer.
		//
		// It returns an integer, so we explicitly convert it to a char because
		// we're treating Serial input as text.
		char c =
				static_cast<char>(Serial.read());

		// Arduino Serial Monitor normally sends \n, \r, or both when Enter is
		// pressed.
		//
		// Either means:
		//
		//   "The command is complete."
		if (c == '\n' || c == '\r')
		{

			// Ignore blank lines.
			if (serialCommandBuffer.length() > 0)
			{

				processCommand(serialCommandBuffer);

				// Empty our command-building buffer so it is ready for the next
				// command.
				serialCommandBuffer = "";
			}

			continue;
		}

		// There is no legitimate BMW Solar Logger command anywhere near 80
		// characters long.
		//
		// This prevents an accidental paste or corrupted stream from allowing the
		// String to grow indefinitely.
		if (serialCommandBuffer.length() >= 80)
		{

			Serial.println(
					"[ERROR] Serial command exceeded 80 characters. Buffer cleared.");

			serialCommandBuffer = "";

			continue;
		}

		// Ordinary character:
		//
		// append it to the command currently being constructed.
		serialCommandBuffer += c;
	}
}

// ============================================================================
// HEARTBEAT
// ============================================================================

void printHeartbeat()
{
	// A shut-down INA228 still answers I2C reads, but the values it returns are
	// left over from before shutdown. Printing them next to "ALL OK" would be
	// presenting stale registers as a live measurement.
	if (inaShutdownActive)
	{
		Serial.println(
				"[HEARTBEAT] INA228 is in SHUTDOWN. No conversions are running and "
				"no values are being updated.");
		return;
	}

	SensorReading reading;

	if (!readSensor(reading, false))
	{
		Serial.println(
				"[HEARTBEAT] ERROR: INA228 read failed");
		return;
	}

	unsigned long elapsedMs =
			millis() - intervalStartMs;

	Serial.print("[HEARTBEAT] ALL OK");

	Serial.print(" | experiment ");
	Serial.print(experimentId);

	if (sleepPowerTestRunning)
	{
		// "next interval N" would be a lie here: no interval is going to close
		// while accounting is suspended. Report the sleep test's clock instead.
		Serial.print(" | SLEEP TEST cycle ");
		Serial.print(rtcSleepTestCycle);

		Serial.print(" | awake ");
		Serial.print(
				(millis() - sleepPowerTestAwakeStartedMs) / 1000.0, 1);
		Serial.print(" s of ");
		Serial.print(SLEEP_POWER_TEST_AWAKE_MS / 1000UL);

		Serial.print(" s | interval accounting SUSPENDED | ");
	}
	else if (intervalAccountingSuspended())
	{
		// The test is over but accounting could not safely resume. Saying
		// "next interval N" here would promise an interval that will not close.
		Serial.print(" | interval accounting SUSPENDED (");
		Serial.print(intervalAccountingSuspendReason());
		Serial.print(") | ");
	}
	else
	{
		Serial.print(" | next interval ");
		Serial.print(completedInterval + 1);

		Serial.print(" | elapsed ");
		Serial.print(elapsedMs / 1000.0, 1);
		Serial.print(" s | ");
	}

	Serial.print(reading.voltage_V, 4);
	Serial.print(" V | ");

	Serial.print(reading.current_mA, 3);
	Serial.print(" mA | ");

	Serial.print(reading.power_mW, 3);
	Serial.println(" mW");
}

// ============================================================================
// CLOSE ONE 60-SECOND MEASUREMENT INTERVAL
// ============================================================================

bool closeMeasurementInterval()
{
	unsigned long endMs =
			millis();

	unsigned long elapsedMs =
			endMs - intervalStartMs;

	double elapsedSeconds =
			elapsedMs / 1000.0;

	Serial.println();

	Serial.println(
			"============================================================");

	Serial.println(
			"[LOGGER] INTERVAL TIMER EXPIRED");

	Serial.print(
			"[LOGGER] Actual elapsed time = ");

	Serial.print(elapsedSeconds, 3);

	Serial.println(
			" seconds");

	// --------------------------------------------------------------------------
	// Capture instantaneous end-of-interval measurement.
	// --------------------------------------------------------------------------

	Serial.println();

	Serial.println(
			"[INA228] Capturing end-of-interval measurements...");

	SensorReading reading;

	if (!readSensor(reading, false))
	{
		Serial.println(
				"[ERROR] Could not close interval because end measurement failed.");

		return false;
	}

	// --------------------------------------------------------------------------
	// Read hardware accumulation BEFORE resetting it.
	// --------------------------------------------------------------------------

	Serial.println(
			"[INA228] Reading accumulated CHARGE...");

	double intervalCharge_mAh;

	if (!readAccumulatedCharge_mAh(intervalCharge_mAh))
	{
		Serial.println(
				"[ERROR] Could not read accumulated CHARGE.");

		return false;
	}

	Serial.println(
			"[INA228] Reading accumulated ENERGY...");

	double intervalEnergy_mWh;

	if (!readAccumulatedEnergy_mWh(intervalEnergy_mWh))
	{
		Serial.println(
				"[ERROR] Could not read accumulated ENERGY.");

		return false;
	}

	Serial.println(
			"[INA228] Closing completed interval.");

	// --------------------------------------------------------------------------
	// Reset INA accumulators so they begin measuring the NEXT interval.
	// --------------------------------------------------------------------------

	if (!resetInaAccumulators())
	{
		Serial.println(
				"[ERROR] New interval was NOT started because accumulator reset failed.");

		return false;
	}

	// Start the next software interval timer.
	intervalStartMs = millis();

	Serial.println(
			"[INA228] New accumulation interval started.");

	// --------------------------------------------------------------------------
	// Update software totals.
	// --------------------------------------------------------------------------

	completedInterval++;

	// Counted here, at the actual close, so the figure cannot depend on when any
	// global was initialized.
	if (hostSessionHeld)
	{
		hostSessionIntervalCloses++;
	}

	runningCharge_mAh += intervalCharge_mAh;

	runningEnergy_mWh += intervalEnergy_mWh;

	// CHARGE tells us the total current integrated over the interval.
	//
	// Rearranging that lets us calculate the average current during the minute.
	double averageCurrent_mA =
			intervalCharge_mAh *
			3600.0 /
			elapsedSeconds;

	// Same idea for ENERGY and average power.
	double averagePower_mW =
			intervalEnergy_mWh *
			3600.0 /
			elapsedSeconds;

	// --------------------------------------------------------------------------
	// Diagnostic shunt read.
	// --------------------------------------------------------------------------

	SensorReading diagnosticReading;

	if (!readSensor(diagnosticReading, true))
	{
		Serial.println(
				"[ERROR] Interval captured, but VSHUNT diagnostics failed.");

		diagnosticReading = reading;
	}

	double shuntPower_mW =
			(diagnosticReading.shuntVoltage_mV / 1000.0) *
			(diagnosticReading.current_mA / 1000.0) *
			1000.0;

	double shuntRangePercent =
			fabs(
					diagnosticReading.shuntVoltage_mV /
					1000.0) /
			VSHUNT_FULL_SCALE_V *
			100.0;

	// --------------------------------------------------------------------------
	// Human-readable interval output.
	// --------------------------------------------------------------------------

	Serial.println();

	Serial.println(
			"------------------------------------------------------------");

	Serial.print("EXPERIMENT #");
	Serial.println(experimentId);

	Serial.print("INTERVAL #");
	Serial.println(completedInterval);

	Serial.print("Actual interval:     ");
	Serial.print(elapsedSeconds, 3);
	Serial.println(" seconds");

	Serial.println();

	Serial.print("Ending voltage:      ");
	Serial.print(reading.voltage_V, 4);
	Serial.println(" V");

	Serial.print("Current right now:   ");
	Serial.print(reading.current_mA, 3);
	Serial.println(" mA");

	Serial.print("Power right now:     ");
	Serial.print(reading.power_mW, 3);
	Serial.println(" mW");

	Serial.print("Temperature:         ");
	Serial.print(reading.temperature_C, 2);
	Serial.println(" C");

	Serial.println();

	Serial.print("Raw VSHUNT counts:   ");
	Serial.println(diagnosticReading.rawVshuntCounts);

	Serial.print("Shunt voltage:       ");
	Serial.print(diagnosticReading.shuntVoltage_mV, 6);
	Serial.println(" mV");

	Serial.print("VSHUNT/R current:    ");
	Serial.print(diagnosticReading.currentFromShunt_mA, 3);
	Serial.println(" mA");

	Serial.print("CURRENT register:    ");
	Serial.print(diagnosticReading.current_mA, 3);
	Serial.println(" mA");

	Serial.print("Current difference:  ");
	Serial.print(
			diagnosticReading.current_mA -
					diagnosticReading.currentFromShunt_mA,
			3);
	Serial.println(" mA");

	Serial.println();

	Serial.print("Interval charge:     ");
	Serial.print(intervalCharge_mAh, 6);
	Serial.println(" mAh");

	Serial.print("Interval energy:     ");
	Serial.print(intervalEnergy_mWh, 6);
	Serial.println(" mWh");

	Serial.print("Average current:     ");
	Serial.print(averageCurrent_mA, 3);
	Serial.println(" mA");

	Serial.print("Average power:       ");
	Serial.print(averagePower_mW, 3);
	Serial.println(" mW");

	Serial.println();

	Serial.print("RUNNING charge:      ");
	Serial.print(runningCharge_mAh, 6);
	Serial.println(" mAh");

	Serial.print("RUNNING energy:      ");
	Serial.print(runningEnergy_mWh, 6);
	Serial.println(" mWh");

	Serial.print("Shunt power:         ");
	Serial.print(shuntPower_mW, 3);
	Serial.println(" mW");

	Serial.print("Shunt range used:    ");
	Serial.print(shuntRangePercent, 3);
	Serial.println(" %");

	Serial.println(
			"------------------------------------------------------------");

	// --------------------------------------------------------------------------
	// Machine-readable CSV data.
	// --------------------------------------------------------------------------

	printCsvRow(
			completedInterval,
			elapsedSeconds,
			reading,
			intervalCharge_mAh,
			intervalEnergy_mWh,
			averageCurrent_mA,
			averagePower_mW);

	// --------------------------------------------------------------------------
	// Persist this completed interval.
	//
	// Currently this happens EVERY interval.
	// --------------------------------------------------------------------------

	Serial.println(
			"[NVS] CHECKPOINT REQUIRED");

	if (!saveCheckpoint())
	{
		Serial.println(
				"[ERROR] Measurement succeeded, but NVS checkpoint failed.");

		Serial.println(
				"[ERROR] CSV data above is valid, but power-loss recovery state "
				"is not guaranteed.");
	}

	return true;
}

// ============================================================================
// AUTONOMOUS BENCH TEST: NVS SETTINGS
// ============================================================================

// Read ONLY the experiment id, read-only and quiet.
//
// The autonomous timer-wake path branches at the top of setup(), before
// loadCheckpoint() has ever run, so the experimentId global is still its
// startup value of zero. Records built there were stamped exp=0 while the
// board was actually running experiment 2. Confirmed on hardware 2026-09-11
// from the first 65 stored records.
//
// loadCheckpoint() cannot simply be called instead: it is loud enough to
// distort the wake timings the bench test exists to measure, and it writes
// four globals that belong to the host-tethered logger.
//
// Returns true when the id is KNOWN, false when it cannot be determined.
// A false return must never be turned into experiment 0 by the caller.
//
// The schema handling below deliberately mirrors loadCheckpoint(). If that
// function's rules change, this one has to change with it, or the same board
// will report two different experiment ids depending on which path ran.
bool loadExperimentIdOnly(uint32_t &outExperimentId)
{
	// true = READ ONLY. Nothing here writes NVS or touches I2C.
	if (!preferences.begin(NVS_NAMESPACE, true))
	{
		// This used to return true with an id of 0, on the reasoning that a
		// read-only open fails when the namespace has never been created and that
		// zero is therefore the real answer. That reasoning was wrong twice.
		//
		// First, a failed nvs_open covers every other reason the read could fail,
		// and the Arduino Preferences API does not distinguish them - so the code
		// was asserting something it could not know.
		//
		// Second, on the path that matters the benign case cannot occur. Arming is
		// a prerequisite for any autonomous wake, and saveAutonomousTestArmed()
		// opens the namespace read/write, which CREATES it. By the time a timer
		// wake calls this function the namespace necessarily exists, so a failure
		// here is a genuine fault and never a fresh board.
		//
		// D-024: when context cannot be loaded, the record is marked
		// EXPERIMENT_UNKNOWN. It never writes a plausible default.
		Serial.println(
				"[NVS] ERROR: Could not open the namespace to read the experiment id.");
		return false;
	}

	if (!preferences.isKey("schema"))
	{
		preferences.end();
		outExperimentId = 0;
		return true;
	}

	uint32_t schema = preferences.getUInt("schema", 0);

	if (schema == 1)
	{
		// loadCheckpoint() treats legacy schema 1 data as experiment 0 by
		// definition. Same answer here, for the same reason.
		preferences.end();
		outExperimentId = 0;
		return true;
	}

	if (schema != NVS_SCHEMA_VERSION)
	{
		preferences.end();
		return false;
	}

	if (!preferences.isKey("experiment"))
	{
		preferences.end();
		return false;
	}

	outExperimentId = preferences.getUInt("experiment", 0);
	preferences.end();
	return true;
}

bool saveAutonomousTestArmed(bool armed)
{
	if (!preferences.begin(NVS_NAMESPACE, false))
	{
		Serial.println("[ERROR] Could not open NVS for autonomous test flag.");
		return false;
	}

	size_t written = preferences.putBool(NVS_AUTO_TEST_KEY, armed);
	preferences.end();

	if (written != sizeof(bool))
	{
		Serial.println("[ERROR] Failed to write autonomous test NVS flag.");
		return false;
	}

	return true;
}

bool loadAutonomousSettings()
{
	if (!preferences.begin(NVS_NAMESPACE, true))
	{
		autonomousTestArmed = false;
		autonomousIntervalSeconds = AUTO_INTERVAL_SECONDS_DEFAULT;
		return true;
	}

	autonomousTestArmed =
			preferences.isKey(NVS_AUTO_TEST_KEY)
					? preferences.getBool(NVS_AUTO_TEST_KEY, false)
					: false;

	autonomousIntervalSeconds =
			preferences.isKey(NVS_AUTO_INTERVAL_KEY)
					? preferences.getUInt(NVS_AUTO_INTERVAL_KEY,
																AUTO_INTERVAL_SECONDS_DEFAULT)
					: AUTO_INTERVAL_SECONDS_DEFAULT;

	preferences.end();

	// A stored value outside the supported range would silently change the
	// cadence the test believes it is running at.
	if (autonomousIntervalSeconds < AUTO_INTERVAL_SECONDS_MIN ||
			autonomousIntervalSeconds > AUTO_INTERVAL_SECONDS_MAX)
	{

		Serial.print("[ERROR] Stored autonomous interval is out of range: ");
		Serial.println(autonomousIntervalSeconds);
		Serial.print("[ERROR] Falling back to the default of ");
		Serial.print(AUTO_INTERVAL_SECONDS_DEFAULT);
		Serial.println(" seconds.");

		autonomousIntervalSeconds = AUTO_INTERVAL_SECONDS_DEFAULT;
	}

	return true;
}

bool saveAutonomousInterval(uint32_t seconds)
{
	if (!preferences.begin(NVS_NAMESPACE, false))
	{
		Serial.println("[ERROR] Could not open NVS for autonomous interval.");
		return false;
	}

	size_t written = preferences.putUInt(NVS_AUTO_INTERVAL_KEY, seconds);
	preferences.end();

	if (written != sizeof(uint32_t))
	{
		Serial.println("[ERROR] Failed to write autonomous interval to NVS.");
		return false;
	}

	return true;
}

// Reserve a block of sequence numbers before using any of them. A crash
// inside a reserved block abandons the block: the next boot starts past it,
// producing a visible GAP rather than a silent REUSE.
bool reserveSequenceBlock(uint32_t fromSeq)
{
	uint32_t newHighWater = fromSeq + AUTO_SEQ_BLOCK;

	if (!preferences.begin(NVS_NAMESPACE, false))
	{
		Serial.println("[ERROR] Could not open NVS to reserve a sequence block.");
		return false;
	}

	size_t written = preferences.putUInt(NVS_AUTO_SEQ_HW_KEY, newHighWater);
	preferences.end();

	if (written != sizeof(uint32_t))
	{
		Serial.println("[ERROR] Failed to reserve a sequence block in NVS.");
		return false;
	}

	rtcAutoSeqHighWater = newHighWater;
	return true;
}

// Reserve only when the existing reservation does not already cover the
// sequence about to be used.
//
// The previous code called reserveSequenceBlock() unconditionally on every arm
// and every cold boot, so each restart burned a fresh 64-wide block whether or
// not the previous one had been used. Two cold boots after an eight-record run
// was enough to push the next sequence from 9 to 131.
// announceNoOp must be false on the timer-wake path. That path measures
// "total work excluding Serial", and a Serial.print() inside the measured
// region would inflate the number the whole bench test exists to produce.
bool ensureSequenceReservation(uint32_t nextSeq, bool announceNoOp)
{
	if (nextSeq < rtcAutoSeqHighWater)
	{
		if (announceNoOp)
		{
			Serial.print("[STORAGE] Sequence reservation already covers ");
			Serial.print(nextSeq);
			Serial.print("; reserved through ");
			Serial.print(rtcAutoSeqHighWater - 1);
			Serial.println(". No NVS write needed.");
		}
		return true;
	}

	return reserveSequenceBlock(nextSeq);
}

uint32_t loadSequenceHighWater()
{
	if (!preferences.begin(NVS_NAMESPACE, true))
	{
		return 0;
	}

	uint32_t value =
			preferences.isKey(NVS_AUTO_SEQ_HW_KEY)
					? preferences.getUInt(NVS_AUTO_SEQ_HW_KEY, 0)
					: 0;

	preferences.end();
	return value;
}

// Boot id identifies one continuous power-on session. It increments on cold
// boot only; a deep-sleep wake keeps the RTC-retained value.
uint32_t nextBootId()
{
	if (!preferences.begin(NVS_NAMESPACE, false))
	{
		Serial.println(
				"[ERROR] Could not open NVS for boot id. Using 0, which is not "
				"unique.");
		return 0;
	}

	uint32_t bootId =
			preferences.isKey(NVS_BOOT_ID_KEY)
					? preferences.getUInt(NVS_BOOT_ID_KEY, 0)
					: 0;

	bootId++;
	size_t written = preferences.putUInt(NVS_BOOT_ID_KEY, bootId);
	preferences.end();

	if (written != sizeof(uint32_t))
	{
		Serial.println("[ERROR] Failed to persist boot id.");
	}

	return bootId;
}

// ============================================================================
// AUTONOMOUS BENCH TEST: RECORD HELPERS
// ============================================================================

uint32_t autoRecordCrc(const AutoRecord &record)
{
	// CRC covers every byte except the trailing crc32 field itself.
	return esp_rom_crc32_le(
			0,
			reinterpret_cast<const uint8_t *>(&record),
			AUTO_RECORD_SIZE - sizeof(uint32_t));
}

bool autoRecordValid(const AutoRecord &record)
{
	if (record.magic != AUTO_RECORD_MAGIC)
	{
		return false;
	}

	if (record.version != AUTO_RECORD_VERSION)
	{
		return false;
	}

	return record.crc32 == autoRecordCrc(record);
}

// True when this record's wake could not read the instantaneous snapshot.
//
// Recognized by the impossible sentinel rather than by a flag bit, because all
// eight flag bits are assigned and the 72-byte layout is frozen. Testing
// bus_uV alone is enough - the four fields are written together - but all four
// are checked so a partially-written record from some future path cannot read
// as a valid snapshot.
bool autoRecordSnapshotUnread(const AutoRecord &record)
{
	return record.bus_uV == AUTO_SNAPSHOT_BUS_UV_UNREAD &&
				 record.avg_current_uA == AUTO_SNAPSHOT_SIGNED_UNREAD &&
				 record.avg_power_uW == AUTO_SNAPSHOT_SIGNED_UNREAD &&
				 record.temp_mC == AUTO_SNAPSHOT_SIGNED_UNREAD;
}

const char *autoTimeQualityName(uint8_t flags)
{
	switch (flags & AUTO_FLAG_TIME_QUALITY_MASK)
	{
	case AUTO_TIME_UNKNOWN:
		return "UNKNOWN";
	case AUTO_TIME_SYNCHRONIZED:
		return "SYNCHRONIZED";
	case AUTO_TIME_HOLDOVER:
		return "HOLDOVER";
	default:
		return "RESERVED";
	}
}

void printAutoRecord(const AutoRecord &record)
{
	Serial.print("seq=");
	Serial.print(record.seq);

	Serial.print(" boot=");
	Serial.print(record.boot_id);

	Serial.print(" exp=");
	if (record.flags & AUTO_FLAG_EXPERIMENT_UNKNOWN)
	{
		Serial.print("UNKNOWN");
	}
	else
	{
		Serial.print(record.experiment_id);
	}

	Serial.print(" elapsed_ms=");
	Serial.print(record.session_elapsed_ms);

	Serial.print(" interval_ms=");
	Serial.print(record.interval_ms);

	// The snapshot fields carry impossible sentinels when readSensor() failed
	// during the wake that wrote this record. Printing them as numbers would show
	// 4294.97 V and -2147 A, which is at least obviously wrong - but naming the
	// condition costs one branch and removes the need to recognize the values.
	if (autoRecordSnapshotUnread(record))
	{
		Serial.print(" V=SNAPSHOT_UNREAD I_mA=SNAPSHOT_UNREAD");
		Serial.print(" P_mW=SNAPSHOT_UNREAD T_C=SNAPSHOT_UNREAD");
	}
	else
	{
		Serial.print(" V=");
		Serial.print(record.bus_uV / 1000000.0, 6);

		Serial.print(" I_mA=");
		Serial.print(record.avg_current_uA / 1000.0, 3);

		Serial.print(" P_mW=");
		Serial.print(record.avg_power_uW / 1000.0, 3);

		Serial.print(" T_C=");
		Serial.print(record.temp_mC / 1000.0, 3);
	}

	Serial.print(" dQ_uAh=");
	Serial.print(record.interval_charge_uAh);

	Serial.print(" dE_uWh=");
	Serial.print(record.interval_energy_uWh);

	Serial.print(" Qsum_uAh=");
	Serial.print(static_cast<long long>(record.running_charge_uAh));

	Serial.print(" Esum_uWh=");
	Serial.print(static_cast<long long>(record.running_energy_uWh));

	Serial.print(" time=");
	Serial.print(autoTimeQualityName(record.flags));

	Serial.print(" epoch=");
	Serial.print(record.epoch_s);

	Serial.print(" flags=0x");
	Serial.print(record.flags, HEX);

	// Decoded inline so a dump is readable without a lookup table. Bits 1-0 are
	// the time-quality field and are already printed above as time=.
	Serial.print("[");

	bool firstName = true;

	if (record.flags & AUTO_FLAG_FIRST_AFTER_BOOT)
	{
		Serial.print("FIRST_AFTER_BOOT");
		firstName = false;
	}

	if (record.flags & AUTO_FLAG_ACCUM_SUSPECT)
	{
		if (!firstName)
			Serial.print(",");
		Serial.print("ACCUM_SUSPECT");
		firstName = false;
	}

	if (record.flags & AUTO_FLAG_INTERVAL_ODD)
	{
		if (!firstName)
			Serial.print(",");
		Serial.print("INTERVAL_ODD");
		firstName = false;
	}

	if (record.flags & AUTO_FLAG_INA_MATHOF)
	{
		if (!firstName)
			Serial.print(",");
		Serial.print("INA_MATHOF");
		firstName = false;
	}

	if (record.flags & AUTO_FLAG_INA_ACCUM_OF)
	{
		if (!firstName)
			Serial.print(",");
		Serial.print("INA_ACCUM_OF");
		firstName = false;
	}

	if (record.flags & AUTO_FLAG_EXPERIMENT_UNKNOWN)
	{
		if (!firstName)
			Serial.print(",");
		Serial.print("EXPERIMENT_UNKNOWN");
		firstName = false;
	}

	if (firstName)
	{
		Serial.print("none");
	}

	Serial.print("]");

	Serial.print(" crc=0x");
	Serial.println(record.crc32, HEX);
}

// ============================================================================
// AUTONOMOUS BENCH TEST: STORAGE
// ============================================================================
//
// LittleFS on the stock "spiffs" partition. No partition-table change, so the
// nvs partition holding experiment state is never touched.
//
// Mount NEVER formats on failure. Formatting would destroy stored history,
// and a first-run unformatted partition is indistinguishable from a corrupted
// one at mount time. Initializing is an explicit operator action:
// LOGGER STORAGE CLEAR YES.
// ============================================================================

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
			Serial.println(
					"[STORAGE] ERROR: Short read while scanning the log. Treating the "
					"remainder as an invalid tail.");
			break;
		}

		if (!autoRecordValid(record))
		{
			// Everything from here on is untrusted. Do not keep scanning past a
			// bad record and pretend later ones are fine.
			scan.invalidRecords++;
			break;
		}

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

	scan.trailingBytes = scan.fileBytes - scan.validBytes;
	return scan;
}

// Rewrite the log to exactly its valid prefix.
//
// The Arduino File API exposes no truncate, so this copies the valid prefix
// to a temporary file and renames it. That is O(file size), but it only runs
// when a corrupt tail was actually found, which should be rare.
bool autoStorageTruncateToValid(const AutoLogScan &scan)
{
	Serial.print("[STORAGE] Rewriting the log to its valid prefix: ");
	Serial.print(static_cast<unsigned long>(scan.validBytes));
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

	while (copied < scan.validBytes)
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

// Boot recovery. Returns the sequence to use for the next record.
//
// Nothing here is silent: every discarded byte is counted and reported.
uint32_t autoStorageRecover()
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
		Serial.print(
				"[STORAGE] WARNING: Found an invalid record at offset ");
		Serial.print(static_cast<unsigned long>(scan.validBytes));
		Serial.println(". Magic, version, or CRC did not match.");
	}

	if (scan.trailingBytes > 0)
	{
		Serial.print("[STORAGE] Discarding ");
		Serial.print(static_cast<unsigned long>(scan.trailingBytes));
		Serial.println(" trailing bytes that could not be validated.");

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
	const bool logIntact = scan.fileExists && scan.validRecords > 0 &&
												 !scan.partialTail && !scan.seqOutOfOrder &&
												 scan.invalidRecords == 0 && scan.trailingBytes == 0;

	uint32_t fromLog = (scan.validRecords > 0) ? scan.lastSeq + 1 : 1;

	// Cached so the reservation logic can see it without a second NVS read.
	rtcAutoSeqHighWater = loadSequenceHighWater();

	// One past the reservation, not at it. The reservation covers up to
	// highWater - 1, so this deliberately skips one extra number rather than
	// risk landing on a sequence that a lost record might have used.
	uint32_t fromNvs = rtcAutoSeqHighWater + 1;

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

	// Test-local running totals continue from the last valid record so a
	// reboot does not restart them at zero.
	rtcAutoRunChargeUAh = scan.lastRunChargeUAh;
	rtcAutoRunEnergyUWh = scan.lastRunEnergyUWh;

	return next;
}

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

// ============================================================================
// AUTONOMOUS BENCH TEST: NON-DESTRUCTIVE INA VALIDATION
// ============================================================================
//
// Timer-wake validation. This function performs READS ONLY.
//
// It must never write CONFIG, ADC_CONFIG, or SHUNT_CAL. The INA228 stayed
// powered and converting through the sleep, so it is already configured, and
// writing ADC_CONFIG would interrupt and restart the conversion in progress,
// discarding accumulation that has not yet been committed.
//
// Returns true when the device is present, correctly configured, and still in
// continuous conversion. On failure it prints the exact register values so
// the state is diagnosable rather than merely "wrong".
// ============================================================================

bool validateInaForWake(uint16_t &configOut,
												uint16_t &adcConfigOut,
												uint16_t &shuntCalOut)
{
	bool ok = true;

	uint16_t manufacturer;
	uint16_t device;

	if (!readRegister16(REG_MANUFACTURER_ID, manufacturer) ||
			!readRegister16(REG_DEVICE_ID, device))
	{
		Serial.println("[AUTO] ERROR: Could not read INA228 identity registers.");
		return false;
	}

	if (manufacturer != 0x5449)
	{
		Serial.print("[AUTO] ERROR: Unexpected MANUFACTURER_ID: 0x");
		Serial.println(manufacturer, HEX);
		ok = false;
	}

	if (!readRegister16(REG_CONFIG, configOut) ||
			!readRegister16(REG_ADC_CONFIG, adcConfigOut) ||
			!readRegister16(REG_SHUNT_CAL, shuntCalOut))
	{
		Serial.println("[AUTO] ERROR: Could not read INA228 configuration.");
		return false;
	}

	// ADCRANGE must still be 0 or every current value is scaled wrong.
	if ((configOut & 0x0010) != 0)
	{
		Serial.print("[AUTO] ERROR: ADCRANGE is not 0. CONFIG = 0x");
		Serial.println(configOut, HEX);
		ok = false;
	}

	uint16_t mode = inaModeFromAdcConfig(adcConfigOut);

	if (mode != INA_MODE_CONTINUOUS_ALL)
	{
		Serial.print(
				"[AUTO] ERROR: INA228 is not in continuous conversion. MODE = 0x");
		Serial.println(mode, HEX);
		Serial.println(
				"[AUTO] The interval that just elapsed cannot be trusted: the device "
				"was not converting for all of it.");
		ok = false;
	}

	if (adcConfigOut != ADC_CONFIG_VALUE)
	{
		Serial.print("[AUTO] WARNING: ADC_CONFIG differs from expected. Got 0x");
		Serial.print(adcConfigOut, HEX);
		Serial.print(", expected 0x");
		Serial.println(ADC_CONFIG_VALUE, HEX);
		ok = false;
	}

	if (shuntCalOut != SHUNT_CAL_VALUE)
	{
		Serial.print("[AUTO] ERROR: SHUNT_CAL differs from expected. Got ");
		Serial.print(shuntCalOut);
		Serial.print(", expected ");
		Serial.println(SHUNT_CAL_VALUE);
		Serial.println(
				"[AUTO] Accumulated charge cannot be scaled correctly with an "
				"unexpected calibration constant.");
		ok = false;
	}

	return ok;
}

// ============================================================================
// AUTONOMOUS BENCH TEST: ONE WAKE CYCLE
// ============================================================================
//
// Entered directly from setup() on a deep-sleep timer wake, BEFORE any of the
// normal cold-boot initialization runs. That ordering is the entire point:
// the normal path calls resetInaAccumulators() without ever reading ENERGY or
// CHARGE, which would destroy the interval this test exists to capture.
//
// This function does not return. It ends in deep sleep, or in an explicit
// error stop.
// ============================================================================

// Timing is measured from setupEntryMicros, not from a value captured at the
// call site, so the printed total covers the whole awake period. The path
// argument decides how the number is labeled: a cold-boot total and a
// timer-wake total are not the same measurement and must never look alike.
void autonomousDeepSleepAgain(AutoSleepPath path);

// Returns true when the board must STAY AWAKE, because a host claimed the
// rendezvous or the test was disarmed during it. Returns nothing at all in the
// ordinary case: it ends in deep sleep.
bool runAutonomousWakeCycle()
{
	const uint32_t t0 = micros();

	setAutoState(AUTO_STATE_TIMER_WAKE_MEASUREMENT, "deep-sleep timer fired");

	// --------------------------------------------------------------------------
	// I2C only. No device configuration writes.
	// --------------------------------------------------------------------------
	Wire.begin(D4, D5);
	Wire.setClock(400000);

	const uint32_t tI2cReady = micros();

	uint8_t flags = 0;
	bool intervalValid = true;

	// --------------------------------------------------------------------------
	// Persistent context, BEFORE anything touches the INA228.
	//
	// Read-only NVS. It cannot reset or disturb the CHARGE and ENERGY registers,
	// so it is safe here, and here is where it has to be: the record is built
	// further down and must carry the real experiment id rather than the
	// startup value of the global.
	// --------------------------------------------------------------------------
	uint32_t recordExperimentId = 0;
	const bool experimentIdKnown = loadExperimentIdOnly(recordExperimentId);

	if (!experimentIdKnown)
	{
		recordExperimentId = AUTO_EXPERIMENT_UNKNOWN;
		flags |= AUTO_FLAG_EXPERIMENT_UNKNOWN;
	}

	const uint32_t tContextRead = micros();

	uint16_t configReg = 0;
	uint16_t adcConfigReg = 0;
	uint16_t shuntCalReg = 0;

	if (!validateInaForWake(configReg, adcConfigReg, shuntCalReg))
	{
		// Keep going far enough to store an explicitly-marked-invalid record.
		// Discarding the evidence would be worse than storing it flagged.
		intervalValid = false;
		flags |= AUTO_FLAG_ACCUM_SUSPECT;
	}

	// --------------------------------------------------------------------------
	// READ THE COMPLETED INTERVAL FIRST. Nothing above this line resets it.
	// --------------------------------------------------------------------------
	double intervalCharge_mAh = 0.0;
	double intervalEnergy_mWh = 0.0;

	bool chargeOk = readAccumulatedCharge_mAh(intervalCharge_mAh);
	bool energyOk = readAccumulatedEnergy_mWh(intervalEnergy_mWh);

	const uint32_t tAccumRead = micros();

	if (!chargeOk || !energyOk)
	{
		Serial.println(
				"[AUTO] ERROR: Could not read the accumulated CHARGE/ENERGY for the "
				"completed interval.");
		intervalValid = false;
		flags |= AUTO_FLAG_ACCUM_SUSPECT;
	}

	// DIAG_ALRT must be sampled in the same wake as CHARGE: CHARGEOF clears
	// when CHARGE is read, so the evidence is gone after this point.
	uint16_t diag = 0;
	bool diagOk = readRegister16(REG_DIAG_ALRT, diag);

	if (!diagOk)
	{
		Serial.println("[AUTO] ERROR: Could not read DIAG_ALRT.");
		intervalValid = false;
		flags |= AUTO_FLAG_ACCUM_SUSPECT;
	}
	else
	{
		if (diag & (DIAG_ENERGYOF_MASK | DIAG_CHARGEOF_MASK))
		{
			flags |= AUTO_FLAG_INA_ACCUM_OF;
			intervalValid = false;
		}

		if (diag & DIAG_MATHOF_MASK)
		{
			flags |= AUTO_FLAG_INA_MATHOF;
			intervalValid = false;
		}
	}

	// --------------------------------------------------------------------------
	// Instantaneous snapshot.
	//
	// Zero-initialized deliberately. readSensor() returns false WITHOUT writing
	// any field when an I2C read fails, so an uninitialized struct here would put
	// whatever happened to be on the stack into a CRC-correct record. That is the
	// D-024 failure mode - populated, well-formed, and wrong - applied to the
	// snapshot fields instead of the experiment id.
	// --------------------------------------------------------------------------
	SensorReading reading = {};
	bool snapshotOk = readSensor(reading, false);

	const uint32_t tSnapshot = micros();

	if (!snapshotOk)
	{
		Serial.println("[AUTO] ERROR: Snapshot read failed.");
		Serial.println(
				"[AUTO] The interval charge and energy are still valid, so the record "
				"is stored with the snapshot fields marked UNREAD.");
		intervalValid = false;
	}

	// --------------------------------------------------------------------------
	// Interval duration.
	//
	// Derived from the commanded sleep duration plus measured awake time, so it
	// inherits RTC oscillator drift. It is not an independent measurement of
	// elapsed wall time, and the design documents that limitation.
	// --------------------------------------------------------------------------
	const uint32_t nowElapsedMs =
			rtcAutoSessionElapsedMs + (micros() - t0) / 1000UL;

	uint32_t intervalMs = nowElapsedMs - rtcAutoIntervalStartMs;
	const uint32_t nominalMs = autonomousIntervalSeconds * 1000UL;

	// More than 20% off the configured cadence is worth flagging rather than
	// averaging into the data unremarked.
	if (intervalMs > nominalMs + nominalMs / 5 ||
			intervalMs + nominalMs / 5 < nominalMs)
	{
		flags |= AUTO_FLAG_INTERVAL_ODD;
	}

	// --------------------------------------------------------------------------
	// Build the record.
	// --------------------------------------------------------------------------
	const int32_t intervalChargeUAh =
			static_cast<int32_t>(intervalCharge_mAh * 1000.0);
	const int32_t intervalEnergyUWh =
			static_cast<int32_t>(intervalEnergy_mWh * 1000.0);

	// PROVISIONAL running totals.
	//
	// These are computed here because the record being built needs them, and are
	// committed to RTC only after the append succeeds. Advancing the retained
	// totals first was a real defect: when the append failed the accumulators
	// were deliberately left unreset so the next interval would cover both
	// periods, but the retained totals had already absorbed the first period, so
	// the next wake read both periods out of the CHARGE register and added them
	// on top. Every later record then carried a running total that was too high
	// by one interval, silently and permanently - autoStorageRecover() reseeds
	// these from the last stored record, so a reboot preserved the error.
	//
	// Reads before writes, and commits only after the durable write, is the same
	// ordering rule D-020 applies to the accumulators themselves.
	const int64_t provisionalRunChargeUAh =
			rtcAutoRunChargeUAh + intervalChargeUAh;
	const int64_t provisionalRunEnergyUWh =
			rtcAutoRunEnergyUWh + intervalEnergyUWh;

	if (rtcAutoCycleCount == 0)
	{
		flags |= AUTO_FLAG_FIRST_AFTER_BOOT;
	}

	// Time quality: this bench test never sets the clock, so it is honestly
	// UNKNOWN and epoch_s stays 0 rather than carrying an invented value.
	//
	// Written as an explicit field assignment rather than an OR. AUTO_TIME_UNKNOWN
	// is zero, so an OR would be a no-op that merely looks like it sets
	// something — and would stop working the moment the value changed.
	flags = (flags & ~AUTO_FLAG_TIME_QUALITY_MASK) | AUTO_TIME_UNKNOWN;

	AutoRecord record = {};
	record.magic = AUTO_RECORD_MAGIC;
	record.version = AUTO_RECORD_VERSION;
	record.flags = flags;
	record.seq = rtcAutoNextSeq;
	record.running_charge_uAh = provisionalRunChargeUAh;
	record.running_energy_uWh = provisionalRunEnergyUWh;
	record.experiment_id = recordExperimentId;
	record.boot_id = rtcAutoBootId;
	record.session_elapsed_ms = nowElapsedMs;
	record.epoch_s = 0;
	record.interval_ms = intervalMs;

	if (snapshotOk)
	{
		record.bus_uV = static_cast<uint32_t>(reading.voltage_V * 1000000.0);
		record.avg_current_uA = static_cast<int32_t>(reading.current_mA * 1000.0);
		record.avg_power_uW = static_cast<int32_t>(reading.power_mW * 1000.0);
		record.temp_mC = static_cast<int32_t>(reading.temperature_C * 1000.0);
	}
	else
	{
		// Physically impossible values rather than a plausible zero. See the
		// AUTO_SNAPSHOT_*_UNREAD definitions for why this is a sentinel and not a
		// flag bit.
		record.bus_uV = AUTO_SNAPSHOT_BUS_UV_UNREAD;
		record.avg_current_uA = AUTO_SNAPSHOT_SIGNED_UNREAD;
		record.avg_power_uW = AUTO_SNAPSHOT_SIGNED_UNREAD;
		record.temp_mC = AUTO_SNAPSHOT_SIGNED_UNREAD;
	}

	record.interval_charge_uAh = intervalChargeUAh;
	record.interval_energy_uWh = intervalEnergyUWh;
	record.crc32 = autoRecordCrc(record);

	// --------------------------------------------------------------------------
	// Store it.
	// --------------------------------------------------------------------------
	const uint32_t tBeforeMount = micros();
	bool mounted = autoStorageMount(false);
	const uint32_t tMounted = micros();

	bool appended = false;
	uint32_t tAppended = tMounted;

	if (mounted)
	{
		appended = autoStorageAppend(record);
		tAppended = micros();
	}
	else
	{
		Serial.println(
				"[AUTO] ERROR: Storage is not mounted. The record for this interval "
				"was NOT stored.");
	}

	// --------------------------------------------------------------------------
	// Only now may the accumulators be reset.
	// --------------------------------------------------------------------------
	const uint32_t tBeforeReset = micros();
	bool resetOk = false;

	if (appended)
	{
		resetOk = resetInaAccumulators();
		rtcAutoIntervalStartMs =
				rtcAutoSessionElapsedMs + (micros() - t0) / 1000UL;
	}
	else
	{
		Serial.println(
				"[AUTO] Accumulators NOT reset, because the record was not stored.");
		Serial.println(
				"[AUTO] The next interval will therefore cover both periods rather "
				"than losing this one.");
		Serial.println(
				"[AUTO] Running totals NOT advanced either, so the period this wake "
				"measured is counted exactly once, by the next record that stores.");
	}

	const uint32_t tReset = micros();

	if (appended)
	{
		// Committed here, and only here. Until the append succeeded, the interval
		// this wake measured is still sitting in the INA228 accumulators and will
		// be read again by the next wake; advancing the retained totals before
		// that point is what made the first period count twice.
		rtcAutoRunChargeUAh = provisionalRunChargeUAh;
		rtcAutoRunEnergyUWh = provisionalRunEnergyUWh;

		rtcAutoNextSeq++;
		rtcAutoCycleCount++;

		if (!ensureSequenceReservation(rtcAutoNextSeq, false))
		{
			// reserveSequenceBlock() has already printed the underlying NVS error.
			// Say what it means: the floor that makes a duplicate impossible after
			// an unclean restart was not written.
			Serial.println(
					"[AUTO] ERROR: The sequence reservation was NOT persisted. A crash "
					"before the next successful reservation could reuse a sequence "
					"number; inspect LOGGER STORAGE INFO before trusting the log tail.");
		}
	}

	// --------------------------------------------------------------------------
	// Report. Printing happens after the work so it cannot inflate the timings.
	// --------------------------------------------------------------------------
	const uint32_t tWorkDone = micros();

	Serial.println();
	Serial.println("============================================================");
	Serial.println("[AUTO] Timer wake detected.");
	Serial.println(
			"[AUTO] INA remained powered; using non-destructive wake path.");
	Serial.print("[AUTO] Cycle: ");
	Serial.println(rtcAutoCycleCount);

	if (experimentIdKnown)
	{
		Serial.print("[AUTO] Experiment id (read from NVS this wake): ");
		Serial.println(recordExperimentId);
	}
	else
	{
		Serial.println(
				"[AUTO] ERROR: Could not determine the experiment id from NVS.");
		Serial.println(
				"[AUTO] The record is marked EXPERIMENT UNKNOWN. It does NOT claim "
				"experiment 0.");
		Serial.println(
				"[AUTO] The measurement itself is unaffected; only its attribution "
				"is unknown.");
	}

	Serial.print("[AUTO] CONFIG=0x");
	Serial.print(configReg, HEX);
	Serial.print(" ADC_CONFIG=0x");
	Serial.print(adcConfigReg, HEX);
	Serial.print(" SHUNT_CAL=");
	Serial.println(shuntCalReg);

	Serial.print("[AUTO] DIAG_ALRT=0x");
	Serial.print(diag, HEX);
	Serial.print("  ENERGYOF=");
	Serial.print((diag & DIAG_ENERGYOF_MASK) ? "1" : "0");
	Serial.print(" CHARGEOF=");
	Serial.print((diag & DIAG_CHARGEOF_MASK) ? "1" : "0");
	Serial.print(" MATHOF=");
	Serial.println((diag & DIAG_MATHOF_MASK) ? "1" : "0");

	Serial.println("[AUTO] Reading completed interval BEFORE reset...");

	Serial.print("[AUTO] Interval charge: ");
	Serial.print(intervalCharge_mAh, 9);
	Serial.print(" mAh (");
	Serial.print(intervalChargeUAh);
	Serial.println(" uAh)");

	Serial.print("[AUTO] Interval energy: ");
	Serial.print(intervalEnergy_mWh, 9);
	Serial.print(" mWh (");
	Serial.print(intervalEnergyUWh);
	Serial.println(" uWh)");

	Serial.print("[AUTO] Interval duration: ");
	Serial.print(intervalMs);
	Serial.println(" ms");

	// The consumer rtcAutoCommandedSleepMs never had. It is retained across deep
	// sleep and was previously written and never read, which hid the thing this
	// line now states: the interval duration above is a COMMANDED value plus
	// measured awake time, not a measurement of elapsed wall time. The session
	// clock is advanced by the commanded sleep before sleeping, so RTC oscillator
	// drift is invisible to it by construction.
	//
	// Printing both is what makes the difference observable on the bench. It is
	// not yet a drift measurement: that needs an absolute-time reference the
	// firmware does not have (STORAGE_SYNC_DESIGN.md Section 12, question 3).
	Serial.print("[AUTO] Commanded sleep for that interval: ");
	Serial.print(rtcAutoCommandedSleepMs);
	Serial.println(
			" ms (interval duration is this plus awake time, NOT measured elapsed "
			"time)");

	Serial.print("[AUTO] Record sequence: ");
	Serial.println(record.seq);

	Serial.print("[AUTO] Record CRC: 0x");
	Serial.println(record.crc32, HEX);

	Serial.print("[AUTO] Interval valid: ");
	Serial.println(intervalValid ? "YES" : "NO");

	if (appended)
	{
		Serial.println("[AUTO] Durable append verified: ALL OK");
	}
	else
	{
		Serial.println("[AUTO] NOT ALL OK - durable append failed.");
	}

	if (appended)
	{
		Serial.println("[AUTO] Resetting INA accumulators for next interval...");
		Serial.println(resetOk ? "[AUTO] Reset verified: ALL OK"
													 : "[AUTO] NOT ALL OK - accumulator reset failed.");
	}

	Serial.println("[AUTO] --- Record ---");
	Serial.print("[AUTO] ");
	printAutoRecord(record);

	Serial.println("[TIMING] Measured, not estimated:");

	Serial.print("[TIMING] Wake -> I2C ready:      ");
	Serial.print((tI2cReady - t0) / 1000.0, 3);
	Serial.println(" ms");

	Serial.print("[TIMING] NVS context read:       ");
	Serial.print((tContextRead - tI2cReady) / 1000.0, 3);
	Serial.println(" ms");

	Serial.print("[TIMING] INA validate+accum read:");
	Serial.print((tAccumRead - tContextRead) / 1000.0, 3);
	Serial.println(" ms");

	Serial.print("[TIMING] INA snapshot complete:  ");
	Serial.print((tSnapshot - tAccumRead) / 1000.0, 3);
	Serial.println(" ms");

	Serial.print("[TIMING] LittleFS mount:         ");
	Serial.print((tMounted - tBeforeMount) / 1000.0, 3);
	Serial.println(" ms");

	Serial.print("[TIMING] Record append total:    ");
	Serial.print((tAppended - tMounted) / 1000.0, 3);
	Serial.println(" ms");

	Serial.print("[TIMING]   open:                  ");
	Serial.print(autoAppendOpenUs / 1000.0, 3);
	Serial.println(" ms");

	Serial.print("[TIMING]   write:                 ");
	Serial.print(autoAppendWriteUs / 1000.0, 3);
	Serial.println(" ms");

	Serial.print("[TIMING]   flush+close:           ");
	Serial.print(autoAppendFlushCloseUs / 1000.0, 3);
	Serial.println(" ms");

	Serial.print("[TIMING] Accumulator reset:      ");
	Serial.print((tReset - tBeforeReset) / 1000.0, 3);
	Serial.println(" ms");

	Serial.print("[TIMING] Total work (no Serial): ");
	Serial.print((tWorkDone - t0) / 1000.0, 3);
	Serial.println(" ms");

	Serial.print("[TIMING] Wake cycle entered at:  ");
	Serial.print((t0 - setupEntryMicros) / 1000.0, 3);
	Serial.println(" ms after setup() entry");

	const uint32_t measurementWorkUs = tWorkDone - t0;

	// --------------------------------------------------------------------------
	// Only now, with the record durably stored and the accumulators reset, is it
	// safe to spend time offering the board to a host.
	// --------------------------------------------------------------------------
	const uint32_t tRendezvousStart = micros();

	const bool stayAwake = autonomousUsbRendezvous(appended);

	const uint32_t rendezvousUs = micros() - tRendezvousStart;

	// Printed separately from the measurement work so the cost of the rendezvous
	// is visible on its own. It is expected to dominate the wake, and it is what
	// a later deployment mode would remove.
	Serial.print("[TIMING] Measurement work:       ");
	Serial.print(measurementWorkUs / 1000.0, 3);
	Serial.println(" ms");

	Serial.print("[TIMING] USB rendezvous:         ");
	Serial.print(rendezvousUs / 1000.0, 3);
	Serial.println(" ms");

	Serial.print("[TIMING] Awake so far:           ");
	Serial.print((micros() - setupEntryMicros) / 1000.0, 3);
	Serial.println(" ms");

	if (stayAwake)
	{
		// setup() continues into its ordinary initialization from here, so the
		// board comes up as a normal tethered logger.
		return true;
	}

	setAutoState(AUTO_STATE_DEEP_SLEEP_PENDING, "rendezvous unclaimed");

	autonomousDeepSleepAgain(AUTO_SLEEP_TIMER_WAKE);
	return false;
}

// ============================================================================
// AUTONOMOUS TIMING (D-032, D-033, D-034, D-046)
// ============================================================================
//
// Two RTC-retained values carry autonomous time across deep sleep:
//
//   rtcAutoSessionElapsedMs   the session time at one known instant
//   rtcAutoIntervalStartMs    the session time of the current interval's
//                             boundary, where its accumulators were reset
//
// Session time NOW is the retained value plus the local time since the instant
// it was exact, and nothing else. That instant depends on who set it last:
//
//   timer wake      the previous sleep set it for this wake's setup() entry:
//                   add the time since setup() entry
//   host handoff    RELEASE or lease expiry set it to the handoff's own now:
//                   add the time since the handoff, hostHandoffAtMs
//   cold boot, arm, RTC loss
//                   the session began on this boot: now is millis()
//
// Until D-046 the handoff path added the time since setup() entry to a value
// the handoff had already advanced to its own now, so the whole awake period
// was counted twice. The first sleep after a RELEASE or lease expiry was short
// by that much, and the next record still claimed a full cadence.
//
// The three functions below are pure: no Arduino calls, no globals, and only
// fixed-width types, because unsigned long is 64 bits on a desktop and 32 here.
// tests/test_autonomous_schedule.py compiles them on the host from this source
// and runs them, so they must stay that way.
// ============================================================================

// Session time at a host handoff. A timer wake continues the retained clock,
// which is exact at setup() entry. A session established on this boot, by a
// cold boot or by the rebuild in the handoff itself, runs on millis().
//
// Both arguments come from millis(), which wraps after 49.7 days. micros()
// wraps after 71.6 minutes, which is shorter than an ordinary logger session.
uint32_t autonomousHandoffElapsedMs(bool continuesRetainedClock,
																		uint32_t retainedElapsedMs,
																		uint32_t sinceSetupEntryMs,
																		uint32_t bootMillisMs)
{
	if (continuesRetainedClock)
	{
		return retainedElapsedMs + sinceSetupEntryMs;
	}

	return bootMillisMs;
}

// Session time at the moment of sleeping, for the path that is sleeping.
uint32_t autonomousElapsedAtSleepMs(AutoSleepPath path,
																		uint32_t retainedElapsedMs,
																		uint32_t awakeMs,
																		uint32_t sinceHandoffMs,
																		uint32_t bootMillisMs)
{
	switch (path)
	{
	case AUTO_SLEEP_TIMER_WAKE:
		// Exact at this wake's setup() entry.
		return retainedElapsedMs + awakeMs;

	case AUTO_SLEEP_HOST_RELEASE:
		// Exact at the handoff. The awake time before the handoff is already
		// inside it; only the time since the handoff is new.
		return retainedElapsedMs + sinceHandoffMs;

	case AUTO_SLEEP_COLD_BOOT:
	case AUTO_SLEEP_ARM_COMMAND:
	case AUTO_SLEEP_RTC_LOST:
		// The session began on this boot, so its clock is this boot's.
		return bootMillisMs;
	}

	// Not reachable with a valid path: every enumerator is handled above, and
	// -Wswitch fails the build when one is added without choosing its clock.
	return bootMillisMs;
}

// D-032: sleep only until the interval deadline. If awake work has reached or
// passed it, start the next interval now and sleep one full cadence, rather
// than wake immediately or underflow into a sleep of about 49 days.
AutoSleepPlan planAutonomousSleep(uint32_t intervalStartMs,
																	uint32_t nowElapsedMs,
																	uint32_t nominalMs)
{
	AutoSleepPlan plan = {};
	const uint32_t targetElapsedMs = intervalStartMs + nominalMs;

	if ((int32_t)(targetElapsedMs - nowElapsedMs) > 0)
	{
		plan.intervalStartMs = intervalStartMs;
		plan.sleepMs = targetElapsedMs - nowElapsedMs;
	}
	else
	{
		// KNOWN DEFECT, not fixed: the boundary moves here but nothing resets the
		// accumulators, so the next record's interval_ms is shorter than the span
		// its charge covers. Every unclaimed wake at LOGGER INTERVAL 10 reaches
		// this. See docs/BACKLOG.md; the strict xfail in
		// tests/test_autonomous_schedule.py states the required property.
		plan.overran = true;
		plan.overrunMs = nowElapsedMs - targetElapsedMs;
		plan.intervalStartMs = nowElapsedMs;
		plan.sleepMs = nominalMs;
	}

	// Advanced by the COMMANDED sleep, so the next wake reads a commanded clock,
	// not a measured one.
	plan.wakeElapsedMs = nowElapsedMs + plan.sleepMs;
	return plan;
}

void autonomousDeepSleepAgain(AutoSleepPath path)
{
	// Measured from the first line of setup(), so this covers the entire awake
	// period. The previous version took its start time from the call site, which
	// on the cold-boot path was a few microseconds before this print and
	// reported "Total wake duration: 0.014 ms" for a multi-second boot.
	const uint32_t awakeUs = micros() - setupEntryMicros;
	const uint32_t awakeMs = awakeUs / 1000UL;
	const uint32_t nowMs = millis();
	const uint32_t currentElapsedMs = autonomousElapsedAtSleepMs(
			path, rtcAutoSessionElapsedMs, awakeMs, nowMs - hostHandoffAtMs, nowMs);
	const uint32_t nominalMs = autonomousIntervalSeconds * 1000UL;
	const AutoSleepPlan plan =
			planAutonomousSleep(rtcAutoIntervalStartMs, currentElapsedMs, nominalMs);

	if (!plan.overran)
	{
		Serial.print("[AUTO] Sleeping until interval deadline; remaining: ");
		Serial.print(plan.sleepMs);
		Serial.println(" ms.");
	}
	else
	{
		// The wake work consumed the entire cadence. Start the next interval now
		// rather than scheduling an immediate wake loop or silently drifting.
		Serial.print("[AUTO] WARNING: Awake work overran the interval deadline by ");
		Serial.print(plan.overrunMs);
		Serial.println(" ms.");
		Serial.println(
				"[AUTO] Starting the next interval now and sleeping one full cadence.");
	}

	const uint32_t sleepMs = plan.sleepMs;

	rtcAutoIntervalStartMs = plan.intervalStartMs;
	rtcAutoCommandedSleepMs = sleepMs;
	rtcAutoSessionElapsedMs = plan.wakeElapsedMs;

	const uint64_t sleepUs = static_cast<uint64_t>(sleepMs) * 1000ULL;

	switch (path)
	{
	case AUTO_SLEEP_TIMER_WAKE:
		Serial.print("[TIMING] Total timer-wake duration (setup entry -> sleep): ");
		Serial.print(awakeUs / 1000.0, 3);
		Serial.println(" ms (includes Serial output)");
		break;

	case AUTO_SLEEP_COLD_BOOT:
		Serial.print("[TIMING] COLD-BOOT startup duration (setup entry -> sleep): ");
		Serial.print(awakeUs / 1000.0, 3);
		Serial.println(" ms");
		Serial.println(
				"[TIMING] NOT a wake timing. It includes the 1500 ms USB delay, the "
				"2000 ms ADC settle, full initialization, and the maintenance "
				"window. Do not compare it against timer-wake numbers.");
		break;

	case AUTO_SLEEP_ARM_COMMAND:
		Serial.print("[TIMING] Arm-to-sleep duration (setup entry -> sleep): ");
		Serial.print(awakeUs / 1000.0, 3);
		Serial.println(" ms");
		Serial.println(
				"[TIMING] NOT a wake timing. The board had been awake and logging "
				"for an unknown time before the arm command arrived.");
		break;

	case AUTO_SLEEP_RTC_LOST:
		Serial.print("[TIMING] Degraded timer-wake duration (setup entry -> sleep): ");
		Serial.print(awakeUs / 1000.0, 3);
		Serial.println(" ms");
		Serial.println(
				"[TIMING] NOT comparable to a healthy timer wake: RTC session state "
				"was lost and the log had to be rescanned.");
		break;

	case AUTO_SLEEP_HOST_RELEASE:
		Serial.print("[TIMING] Host-session duration (setup entry -> sleep): ");
		Serial.print(awakeUs / 1000.0, 3);
		Serial.println(" ms");
		Serial.println(
				"[TIMING] NOT a wake timing. It covers the entire time a host held "
				"the board awake.");
		break;
	}

	Serial.print("[TIMING] micros() at setup entry: ");
	Serial.print(setupEntryMicros);
	Serial.println(" us (bootloader time before setup(), not included above)");

	if (path == AUTO_SLEEP_HOST_RELEASE)
	{
		Serial.println("[AUTO] Entering deep sleep after RELEASE");
	}

	esp_err_t timerResult = esp_sleep_enable_timer_wakeup(sleepUs);

	if (timerResult != ESP_OK)
	{
		Serial.print(
				"[AUTO] ERROR: Could not enable the wake timer. esp_err_t = ");
		Serial.println(static_cast<int>(timerResult));
		Serial.println(
				"[AUTO] Refusing to sleep without a wake source. Stopping the test.");

		autonomousTestRunning = false;
		saveAutonomousTestArmed(false);
		return;
	}

	Serial.print("[AUTO] Entering deep sleep for ");
	Serial.print(sleepMs);
	Serial.println(" ms.");
	Serial.println("============================================================");
	Serial.flush();

	esp_deep_sleep_start();

	Serial.println(
			"[AUTO] ERROR: esp_deep_sleep_start() returned. Deep sleep did not "
			"happen.");
}

// ============================================================================
// USB RENDEZVOUS
// ============================================================================
//
// Opened only AFTER the durable measurement cycle is complete. Measurement
// integrity comes first: nothing here runs while the INA228 accumulators still
// hold an interval that has not been stored.
//
// Returns true when the board must STAY AWAKE, either because a host claimed
// the wake or because the autonomous test was disarmed during the window.
// ============================================================================

bool autonomousUsbRendezvous(bool cycleOk)
{
	setAutoState(AUTO_STATE_USB_RENDEZVOUS,
							 cycleOk ? "record stored" : "record NOT stored");

	hostClaimedRendezvous = false;

	const uint32_t windowStart = millis();
	uint32_t lastTickMs = 0;

	Serial.println();

	if (cycleOk)
	{
		Serial.println("[AUTO] Measurement/store cycle complete: ALL OK");
	}
	else
	{
		Serial.println(
				"[AUTO] NOT ALL OK - the measurement/store cycle failed this wake.");
		Serial.println(
				"[AUTO] Opening the rendezvous anyway so an operator can intervene "
				"rather than leaving the board unreachable.");
	}

	Serial.print("[AUTO] USB rendezvous window: ");
	Serial.print(AUTONOMOUS_USB_RENDEZVOUS_MS);
	Serial.println(" ms");
	Serial.println("[AUTO] Host may claim this wake with LOGGER SESSION HOLD");

	printUsbPresence("at rendezvous open");

	while (true)
	{
		const uint32_t elapsed = millis() - windowStart;

		if (elapsed >= AUTONOMOUS_USB_RENDEZVOUS_MS)
		{
			break;
		}

		handleSerialCommands();

		if (hostClaimedRendezvous)
		{
			Serial.print("[AUTO] Host claimed this wake after ");
			Serial.print(elapsed);
			Serial.println(" ms.");
			Serial.println(
					"[AUTO] Rendezvous timeout CANCELLED. The board stays awake for as "
					"long as the lease is renewed.");
			printActiveTransports();
			return true;
		}

		// LOGGER TEST STOP during the window disarms autonomous mode entirely.
		// Staying awake is then the correct outcome, and it is a different reason
		// from a host claim.
		if (!autonomousTestArmed)
		{
			Serial.println(
					"[AUTO] Autonomous mode was DISARMED during the rendezvous.");
			Serial.println("[AUTO] Staying awake. The board will not sleep again.");
			return true;
		}

		// Roughly 1 Hz. This is the DEVELOPMENT diagnostic block that answers the
		// blocking question: does a timer wake make USB usable to the host at all.
		// Faster than this floods the port a host is still trying to enumerate.
		if (elapsed - lastTickMs >= AUTONOMOUS_RENDEZVOUS_TICK_MS)
		{
			lastTickMs = elapsed;

			Serial.print("[AUTO] Rendezvous: ");
			Serial.print(elapsed / 1000UL);
			Serial.print(" / ");
			Serial.print(AUTONOMOUS_USB_RENDEZVOUS_MS / 1000UL);
			Serial.println(" s");

#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
			Serial.print("[AUTO] USB plugged: ");
			Serial.println(Serial.isPlugged() ? "YES" : "NO");

			Serial.print("[AUTO] CDC connected: ");
			Serial.println(Serial.isConnected() ? "YES" : "NO");
#else
			Serial.println("[AUTO] USB plugged: UNKNOWN (Serial is not HWCDC)");
			Serial.println("[AUTO] CDC connected: UNKNOWN (Serial is not HWCDC)");
#endif

			printActiveTransports();

			Serial.print("[CONNECTION] Host claimed: ");
			Serial.println(anyHostConnected() ? "YES" : "NO");
		}

		delay(5);
	}

	Serial.println("[AUTO] USB rendezvous expired with no host claim.");
	Serial.println("[AUTO] Returning to deep sleep: ALL OK");
	return false;
}

// ============================================================================
// HOST SESSION -> AUTONOMOUS SLEEP
// ============================================================================
//
// The accounting-sensitive transition. Returns which stage failed when it
// refused to prepare the handoff. When sleepNow is false, it returns
// HANDOFF_PREPARED after the clean baseline and state transition so a command
// acknowledgement can be delivered.
//
// The hazard is the tethered interval that is open when the host lets go. Left
// open, whatever charge it holds would roll into the FIRST autonomous record,
// producing an interval made of tethered awake time plus deep-sleep time.
// Discarded, it would be silent charge loss. Closed, it is recorded at its real
// elapsed length and the accumulators are reset, which is simultaneously honest
// and exactly the clean baseline the next autonomous interval needs.
// ============================================================================

const char *handoffStageName(HandoffResult result)
{
	switch (result)
	{
	case HANDOFF_PREPARED:
		return "none - handoff prepared";
	case HANDOFF_INTERVAL_NOT_CLOSED:
		return "closing the open tethered interval";
	case HANDOFF_STORAGE_UNAVAILABLE:
		return "rebuilding autonomous state - storage unavailable";
	}

	return "UNKNOWN";
}

HandoffResult beginAutonomousSleepFromHostSession(const char *reason,
																									bool sleepNow)
{
	Serial.println();
	Serial.println("============================================================");
	Serial.print("[AUTO] Handing the board back to the autonomous cycle: ");
	Serial.println(reason);

	const unsigned long tetheredMs = millis() - intervalStartMs;

	Serial.print("[AUTO] Session lasted ");
	Serial.print((millis() - hostSessionStartedMs) / 1000.0, 3);
	Serial.print(" s; normal 60 s intervals already closed during it: ");
	Serial.println(hostSessionIntervalCloses);

	Serial.print("[AUTO] Closing the open tethered interval first (");
	Serial.print(tetheredMs / 1000.0, 3);
	Serial.println(" s).");
	Serial.println(
			"[AUTO] A short value here is correct when a 60 s interval closed just "
			"before release. Session time is accounted across all of them, not only "
			"this last one.");
	Serial.println(
			"[AUTO] It is recorded at its real elapsed length, not as a full "
			"interval, so no charge is lost and none is double counted.");

	if (!closeMeasurementInterval())
	{
		Serial.println(
				"[AUTO] NOT ALL OK - the tethered interval could not be closed.");
		Serial.println(
				"[AUTO] REFUSING to sleep. The accumulators hold charge that is "
				"accounted nowhere, and sleeping would fold it into the next "
				"autonomous record.");
		Serial.println(
				"[AUTO] Staying awake for diagnosis. Autonomous mode is still ARMED "
				"and will resume after a reset.");

		hostSessionHeld = false;
		hostClaimedRendezvous = false;

		setAutoState(AUTO_STATE_IDLE, "interval close FAILED; refusing to sleep");
		return HANDOFF_INTERVAL_NOT_CLOSED;
	}

	// Decided BEFORE the rebuild below, which sets the magic. Only a timer wake
	// that arrived with valid retained state continues that state's clock. A
	// rebuild starts a new boot_id, so its clock is this boot's millis(); testing
	// the magic after the rebuild would have continued a clock the rebuild had
	// just declared invalid.
	const bool continuesRetainedClock =
			bootWakeupCause == ESP_SLEEP_WAKEUP_TIMER &&
			rtcAutoMagic == AUTO_RTC_MAGIC;

	// A host session can be claimed outside a rendezvous, so the autonomous
	// session state may never have been established. Rebuild it from the durable
	// log rather than sleeping with an unknown sequence.
	if (rtcAutoMagic != AUTO_RTC_MAGIC)
	{
		Serial.println(
				"[AUTO] No valid autonomous session state. Rebuilding it from the "
				"durable log before sleeping.");

		if (!autoStorageMount(true))
		{
			Serial.println(
					"[AUTO] NOT ALL OK - storage unavailable. REFUSING to sleep, "
					"because the next wake would have nowhere to write.");

			setAutoState(AUTO_STATE_IDLE, "storage unavailable; refusing to sleep");
			return HANDOFF_STORAGE_UNAVAILABLE;
		}

		rtcAutoMagic = AUTO_RTC_MAGIC;
		rtcAutoBootId = nextBootId();
		rtcAutoCycleCount = 0;
		rtcAutoNextSeq = autoStorageRecover();
		ensureSequenceReservation(rtcAutoNextSeq, true);
	}

	// The next autonomous interval starts now, from the clean baseline that
	// closeMeasurementInterval() just established. A timer wake already has a
	// session elapsed value retained across sleep; rebasing it to millis() here
	// would make session_elapsed_ms move backward while boot_id stays the same.
	//
	// The retained clock is set to the session time of THIS instant, and
	// hostHandoffAtMs records the instant, so the scheduler adds only the time
	// since the handoff and never the awake time again (D-046). Both readings are
	// millis(): the micros() form used here before wrapped after 71.6 minutes
	// and dropped 4,294,967 ms from the session clock for each wrap.
	const uint32_t handoffAtMs = millis();
	const uint32_t sessionElapsedNow = autonomousHandoffElapsedMs(
			continuesRetainedClock, rtcAutoSessionElapsedMs,
			handoffAtMs - setupEntryMicros / 1000UL, handoffAtMs);
	rtcAutoSessionElapsedMs = sessionElapsedNow;
	rtcAutoIntervalStartMs = sessionElapsedNow;
	hostHandoffAtMs = handoffAtMs;

	autonomousTestRunning = true;

	Serial.print("[AUTO] Next autonomous record will be sequence ");
	Serial.println(rtcAutoNextSeq);

	setAutoState(AUTO_STATE_DEEP_SLEEP_PENDING, reason);

	if (sleepNow)
	{
		autonomousDeepSleepAgain(AUTO_SLEEP_HOST_RELEASE);
	}

	// When sleepNow is false, the caller owns the bounded acknowledgement grace
	// period and will enter deep sleep from the main loop after it expires.
	return HANDOFF_PREPARED;
}

// ----------------------------------------------------------------------------
// Deferred autonomous sleep (D-035, D-044)
// ----------------------------------------------------------------------------
//
// Three functions write pendingAutonomousSleep, and nothing else may:
//
//   armPendingAutonomousSleep()      RELEASE after a prepared handoff, and
//                                    LOGGER AUTONOMOUS ON after arming
//   cancelPendingAutonomousSleep()   an explicit stop, or autonomous mode off
//   servicePendingAutonomousSleep()  the deadline passed: enter the sleep
//
// While a sleep is pending the board is committed to it. hostSessionHold()
// refuses, because a lease granted in the grace would be slept on when the
// grace expires, and the RELEASE handoff has already closed the tethered
// interval and moved the retained session clock to the new autonomous
// boundary.
// ----------------------------------------------------------------------------

// DEEP_SLEEP_PENDING is part of arming, not decoration. autonomousOwnsBoard()
// then answers true for the whole grace, so tethered accounting stays suspended
// and POWER TEST STOP and RESET YES are refused while the accumulators already
// belong to the next autonomous interval. The RELEASE handoff was already in
// that state; LOGGER AUTONOMOUS ON was not, and now is.
void armPendingAutonomousSleep(AutoSleepPath path, const char *reason)
{
	pendingAutonomousSleep = true;
	pendingAutonomousSleepPath = path;
	pendingAutonomousSleepDeadlineMs = millis() + SESSION_RELEASE_ACK_GRACE_MS;

	setAutoState(AUTO_STATE_DEEP_SLEEP_PENDING, reason);

	Serial.print("[AUTO] Deferred autonomous sleep ARMED; entering it in ");
	Serial.print(SESSION_RELEASE_ACK_GRACE_MS);
	Serial.println(" ms, after the command result has been delivered.");
}

void cancelPendingAutonomousSleep(const char *reason)
{
	if (!pendingAutonomousSleep)
	{
		return;
	}

	pendingAutonomousSleep = false;
	setAutoState(AUTO_STATE_IDLE, reason);

	Serial.print("[AUTO] Pending autonomous sleep CANCELED: ");
	Serial.println(reason);
	Serial.println("[AUTO] The board stays awake.");
}

void servicePendingAutonomousSleep()
{
	if (!pendingAutonomousSleep)
	{
		return;
	}

	if (!autonomousTestArmed)
	{
		cancelPendingAutonomousSleep("autonomous mode is OFF");
		return;
	}

	if ((int32_t)(pendingAutonomousSleepDeadlineMs - millis()) > 0)
	{
		return;
	}

	pendingAutonomousSleep = false;
	Serial.println("[AUTO] Deferred sleep deadline reached");
	Serial.println(
			"[AUTO] Command acknowledgement grace elapsed; entering autonomous "
			"sleep.");
	autonomousDeepSleepAgain(pendingAutonomousSleepPath);
}

// ============================================================================
// AUTONOMOUS BENCH TEST: ARM / STOP / STATUS
// ============================================================================

const char *autoStateName(AutoState state)
{
	switch (state)
	{
	case AUTO_STATE_IDLE:
		return "IDLE";
	case AUTO_STATE_COLD_BOOT_MAINTENANCE:
		return "COLD_BOOT_MAINTENANCE";
	case AUTO_STATE_TIMER_WAKE_MEASUREMENT:
		return "TIMER_WAKE_MEASUREMENT";
	case AUTO_STATE_USB_RENDEZVOUS:
		return "USB_RENDEZVOUS";
	case AUTO_STATE_HOST_SESSION:
		return "HOST_SESSION";
	case AUTO_STATE_HOST_SESSION_RELEASED:
		return "HOST_SESSION_RELEASED";
	case AUTO_STATE_HOST_LEASE_EXPIRED:
		return "HOST_LEASE_EXPIRED";
	case AUTO_STATE_DEEP_SLEEP_PENDING:
		return "DEEP_SLEEP_PENDING";
	}

	// Not reachable for a valid enum value, and saying so beats printing a
	// number that looks like a state name.
	return "UNKNOWN";
}

// Every transition prints. Transitions only: this is never called from inside
// a polling loop, so it cannot flood the port.
void setAutoState(AutoState next, const char *reason)
{
	if (autonomousState == next)
	{
		return;
	}

	Serial.print("[AUTO] STATE: ");
	Serial.print(autoStateName(autonomousState));
	Serial.print(" -> ");
	Serial.print(autoStateName(next));
	Serial.print("  (");
	Serial.print(reason);
	Serial.println(")");

	autonomousState = next;
}

bool hostLeaseValid()
{
	if (!hostSessionHeld)
	{
		return false;
	}

	// Unsigned subtraction, so this stays correct across the millis() rollover
	// at 49 days.
	return (int32_t)(hostLeaseDeadlineMs - millis()) > 0;
}

uint32_t hostLeaseRemainingMs()
{
	if (!hostLeaseValid())
	{
		return 0;
	}

	return hostLeaseDeadlineMs - millis();
}

// Report whether a USB host appears to be attached.
//
// INVESTIGATED, NOT DEPENDED ON. On this board Serial is HWCDC (the USB
// Serial/JTAG peripheral), which exposes two different questions:
//
//   isPlugged()    usb_serial_jtag_is_connected(): a timer-based check for USB
//                  start-of-frame packets. The ESP32 core's own source comments
//                  state it has several milliseconds of tolerance and "is known
//                  to flap even on a healthy link".
//
//   isConnected()  stronger: requires isPlugged() AND that the host has
//                  actually clocked bytes out of the TX FIFO. It reads false
//                  whenever no terminal holds the port open, even with USB
//                  physically attached.
//
// Neither answers "can an operator reach me right now" reliably enough to gate
// recovery on, so both are printed as diagnostics only. The guaranteed
// maintenance window is the recovery mechanism. Printing them over several
// bench runs is how we find out whether either is trustworthy enough to use
// later.
void printUsbPresence(const char *label)
{
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
	Serial.print("[AUTO] USB ");
	Serial.print(label);
	Serial.print(": plugged=");
	Serial.print(Serial.isPlugged() ? "YES" : "NO");
	Serial.print(" cdcConnected=");
	Serial.println(Serial.isConnected() ? "YES" : "NO");
#else
	Serial.print("[AUTO] USB ");
	Serial.print(label);
	Serial.println(": not detectable in this build (Serial is not HWCDC).");
#endif
}

// The cold-boot maintenance window.
//
// Returns true if the autonomous test should continue into deep sleep, false
// if the operator stopped it while the window was open.
//
// This exists because handleSerialCommands() runs only from loop(), and an
// armed cold boot never reaches loop(). Without this window the firmware can
// receive a command's bytes and never parse them, which is exactly what
// happened on 2026-09-11.
bool autonomousColdBootMaintenanceWindow()
{
	const uint32_t windowStart = millis();
	uint32_t lastTickMs = 0;

	Serial.println();
	Serial.println("============================================================");
	Serial.println("[AUTO] Persisted autonomous mode found.");
	Serial.print("[AUTO] Cold-boot maintenance window: ");
	Serial.print(AUTONOMOUS_COLD_BOOT_MAINTENANCE_MS / 1000UL);
	Serial.println(" seconds.");
	Serial.println("[AUTO] Send LOGGER TEST STOP to cancel autonomous mode.");
	Serial.println(
			"[AUTO] LOGGER TEST STATUS, LOGGER STORAGE INFO and LOGGER STORAGE DUMP "
			"also work during this window.");
	Serial.println(
			"[AUTO] This window is COLD BOOT ONLY. Timer wakes stay short.");
	Serial.println("============================================================");

	printUsbPresence("at window open");

	while (true)
	{
		const uint32_t elapsed = millis() - windowStart;

		if (elapsed >= AUTONOMOUS_COLD_BOOT_MAINTENANCE_MS)
		{
			break;
		}

		handleSerialCommands();

		// stopAutonomousTest() clears this only after the NVS write succeeds.
		if (!autonomousTestArmed)
		{
			Serial.println();
			Serial.println("[AUTO] Autonomous test STOPPED.");
			Serial.println("[AUTO] Persisted autonomous flag cleared: ALL OK");
			Serial.println(
					"[AUTO] The board will NOT enter deep sleep. Normal logging "
					"continues.");
			return false;
		}

		// A host claimed the board during the maintenance window. Without this
		// the window would run to completion and sleep on top of a live session,
		// silently discarding the claim the host just made and was acknowledged.
		if (anyHostConnected())
		{
			Serial.println();
			Serial.println(
					"[AUTO] A host claimed the board during the maintenance window.");
			Serial.println(
					"[AUTO] Not entering autonomous sleep. Autonomous mode stays ARMED "
					"and resumes when the host releases or stops renewing.");
			printActiveTransports();
			return false;
		}

		// A stop was asked for and the flag is still set, so the NVS write failed
		// and stopAutonomousTest() has already said so. Do not sleep anyway.
		if (autonomousStopRequested)
		{
			Serial.println();
			Serial.println(
					"[AUTO] NOT ALL OK - a stop was requested but the persisted flag "
					"could NOT be cleared.");
			Serial.println(
					"[AUTO] Refusing to enter deep sleep: the operator asked for the "
					"test to stop.");
			Serial.println(
					"[AUTO] WARNING: the test is still armed in NVS and WILL resume on "
					"the next boot. Send LOGGER TEST STOP again.");
			return false;
		}

		if (elapsed - lastTickMs >= AUTONOMOUS_MAINTENANCE_TICK_MS)
		{
			lastTickMs = elapsed;

			const uint32_t remainingS =
					(AUTONOMOUS_COLD_BOOT_MAINTENANCE_MS - elapsed + 999UL) / 1000UL;

			Serial.print("[AUTO] Maintenance window remaining: ");
			Serial.print(remainingS);
			Serial.println(" s");
		}

		delay(10);
	}

	printUsbPresence("at window close");

	Serial.println("[AUTO] Maintenance window expired.");
	Serial.println("[AUTO] Resuming autonomous logger: ALL OK");
	return true;
}

// ============================================================================
// HOST SESSION COMMANDS
// ============================================================================
//
// LOGGER SESSION HOLD is NOT LOGGER TEST STOP, and confusing them would be
// expensive. HOLD keeps the board awake while a host is attached and leaves
// autonomous mode ARMED. STOP persistently disarms the autonomous test.
//
//   autonomous armed = YES, host session held = YES
//
// is a normal, valid state: a host is driving a board that will resume
// autonomous operation the moment the host lets go.
// ============================================================================

HoldResult hostSessionHold()
{
	if (!autonomousTestArmed)
	{
		Serial.println(
				"[SESSION] ERROR: Autonomous mode is not armed, so there is no "
				"autonomous sleep to suspend.");
		Serial.println(
				"[SESSION] The board is already staying awake. No lease was taken.");
		Serial.println("[SESSION] NOT ALL OK - HOLD refused.");
		return HOLD_NOT_ARMED;
	}

	// REFUSE while a deferred autonomous sleep is pending (D-044).
	//
	// RELEASE and LOGGER AUTONOMOUS ON both end in a bounded grace, after which
	// loop() enters deep sleep. Granting a lease here used to answer OK and then
	// sleep on top of it when the grace expired, discarding the new tethered
	// interval and the lease with no RELEASE and no expiry notice.
	//
	// Canceling the pending sleep instead was considered and rejected. The
	// RELEASE handoff has already closed the tethered interval, reset the
	// accumulators, and moved the retained session clock to the new autonomous
	// boundary; taking a session back would need all of that undone. A host that
	// meets this refusal claims the board at the next rendezvous.
	if (pendingAutonomousSleep)
	{
		Serial.println(
				"[SESSION] ERROR: The board is already committed to autonomous sleep "
				"and enters it within the acknowledgement grace.");
		Serial.println(
				"[SESSION] No lease was taken. Claim the board at the next USB "
				"rendezvous.");
		Serial.println(
				"[SESSION] NOT ALL OK - HOLD not granted: autonomous sleep is pending.");
		return HOLD_SLEEP_PENDING;
	}

	const bool renewing = hostSessionHeld;

	hostSessionHeld = true;
	hostLeaseDeadlineMs = millis() + AUTONOMOUS_HOST_LEASE_MS;
	hostLeaseRenewedMs = millis();
	hostClaimedRendezvous = true;

	if (!renewing)
	{
		hostSessionStartedMs = millis();
		hostKeepaliveCount = 0;
		hostSessionIntervalCloses = 0;

		// ------------------------------------------------------------------------
		// Establish the tethered measurement baseline HERE, at the claim.
		//
		// It used to be established at the end of setup(), hundreds of lines and
		// several seconds later, and setup() reset the accumulators again on the
		// way past. Everything measured between the autonomous reset and that
		// second reset was discarded without a word. On 2026-09-11 that silently
		// threw away about 74 seconds of charge and left RELEASE closing a 9 ms
		// interval.
		//
		// Resetting once, here, means the tethered interval starts at the instant
		// the host takes the board and spans the whole session.
		// ------------------------------------------------------------------------
		Serial.println();
		Serial.println(
				"[SESSION] Charge accumulated during the USB rendezvous belongs to no "
				"interval and is being discarded. This is a stated gap, not a silent "
				"loss.");

		if (resetInaAccumulators())
		{
			intervalStartMs = millis();
			lastHeartbeatMs = intervalStartMs;
			lastLiveSampleMs = intervalStartMs;

			hostSessionBaselineEstablished = true;

			Serial.println(
					"[SESSION] Tethered measurement interval baseline established at "
					"the claim: ALL OK");
		}
		else
		{
			// Never pretend the baseline is clean. Without it the eventual close
			// would report charge from an unknown window as if it were the session.
			hostSessionBaselineEstablished = false;

			Serial.println(
					"[SESSION] NOT ALL OK - could not reset the INA accumulators.");
			Serial.println(
					"[SESSION] The tethered interval baseline is NOT clean. Whatever "
					"this session eventually closes will cover an unknown window.");
		}
	}

	Serial.println();

	if (renewing)
	{
		Serial.println("[SESSION] Host session lease RENEWED by HOLD.");
	}
	else
	{
		Serial.println("[SESSION] Host session ACQUIRED.");
		Serial.println(
				"[SESSION] Autonomous deep sleep is suspended while this lease is "
				"valid.");
		Serial.println(
				"[SESSION] Autonomous mode remains ARMED. This is not LOGGER TEST "
				"STOP.");
	}

	Serial.print("[SESSION] Lease valid for ");
	Serial.print(AUTONOMOUS_HOST_LEASE_MS);
	Serial.println(" ms from now.");
	Serial.println(
			"[SESSION] Send LOGGER SESSION KEEPALIVE before it expires, or the board "
			"resumes autonomous sleep on its own.");
	Serial.println("[SESSION] Host session held: ALL OK");

	// The lease is what marks a transport active. USB being electrically
	// present is never enough on its own.
	connectionClaim(CONNECTION_USB);

	// Leaving the autonomous states also un-suspends interval accounting, so
	// ordinary tethered telemetry resumes for the duration of the session.
	autonomousTestRunning = false;
	setAutoState(AUTO_STATE_HOST_SESSION, "LOGGER SESSION HOLD");
	return HOLD_GRANTED;
}

void hostSessionKeepalive()
{
	if (!hostSessionHeld)
	{
		// Never silent. A host that thinks it holds a lease and does not is
		// exactly the state that ends with the board sleeping mid-session.
		Serial.println(
				"[SESSION] ERROR: KEEPALIVE received but no host session is held.");
		Serial.println(
				"[SESSION] The lease may have already expired. Send LOGGER SESSION "
				"HOLD to take a new one.");
		Serial.println("[SESSION] NOT ALL OK - lease NOT renewed.");
		return;
	}

	hostLeaseDeadlineMs = millis() + AUTONOMOUS_HOST_LEASE_MS;
	hostLeaseRenewedMs = millis();
	hostKeepaliveCount++;

	// Deliberately one short line. This runs every few seconds for the entire
	// session, and the acknowledgement still has to be unambiguous for the host
	// waiting on it.
	Serial.print("[SESSION] KEEPALIVE ");
	Serial.print(hostKeepaliveCount);
	Serial.print(": lease renewed for ");
	Serial.print(AUTONOMOUS_HOST_LEASE_MS);
	Serial.println(" ms: ALL OK");
}

void servicePendingAutonomousSleep();

// RELEASE COMPLETED - the only case that returns true, and the only case the
// dispatcher answers OK (D-041, D-044):
//
//   1. a session was held, and it has been ended: lease dropped, USB
//      transport released
//   2. if autonomous mode is armed:
//        the accounting handoff was PREPARED - the tethered interval closed,
//        autonomous session state valid, the next interval's baseline set
//      and the deferred sleep is ARMED with its bounded grace
//
// Entering deep sleep is deliberately NOT part of it. That happens after
// CMD_RESULT has been emitted, which is the whole reason it is deferred (D-035).
// A host's evidence that it happened is the transport disappearing, and a
// vanished port proves nothing on its own.
//
// Not held: returns false, answered NOT_HELD. Held but the handoff failed:
// the session is still over, the board stays awake for diagnosis, returns
// false, answered ERROR.
bool hostSessionRelease()
{
	if (!hostSessionHeld)
	{
		Serial.println(
				"[SESSION] No host session was held. Nothing to release.");

		if (autonomousTestArmed)
		{
			Serial.println(
					"[SESSION] Autonomous mode is armed, so the board is already "
					"operating autonomously or is about to.");
		}

		return false;
	}

	Serial.println();
	Serial.print("[SESSION] Host session release requested after ");
	Serial.print((millis() - hostSessionStartedMs) / 1000UL);
	Serial.print(" s and ");
	Serial.print(hostKeepaliveCount);
	Serial.println(" keepalives.");

	hostSessionHeld = false;
	hostClaimedRendezvous = false;
	hostSessionBaselineEstablished = false;
	connectionRelease(CONNECTION_USB);

	if (!autonomousTestArmed)
	{
		Serial.println(
				"[SESSION] Autonomous mode is NOT armed, so the board stays awake and "
				"keeps logging normally.");
		Serial.println("[SESSION] Released: ALL OK");
		setAutoState(AUTO_STATE_IDLE, "released, autonomous not armed");
		return true;
	}

	Serial.println(
			"[SESSION] Autonomous mode is still ARMED. Resuming the autonomous "
			"sleep cycle.");
	setAutoState(AUTO_STATE_HOST_SESSION_RELEASED, "LOGGER SESSION RELEASE");

	const HandoffResult handoff =
			beginAutonomousSleepFromHostSession("host released the session", false);

	if (handoff != HANDOFF_PREPARED)
	{
		Serial.print("[SESSION] NOT ALL OK - RELEASE did not complete. Failed stage: ");
		Serial.println(handoffStageName(handoff));
		Serial.println(
				"[SESSION] The session is over and its lease is gone, but the board did "
				"NOT hand back to autonomous sleep. It stays awake for diagnosis.");
		Serial.println(
				"[SESSION] Autonomous mode is still ARMED in NVS and resumes after a "
				"reset.");
		return false;
	}

	Serial.println("[SESSION] RELEASE accounting complete");
	armPendingAutonomousSleep(AUTO_SLEEP_HOST_RELEASE, "LOGGER SESSION RELEASE");
	Serial.println("[SESSION] Released: ALL OK");
	return true;
}

void printHostSessionStatus()
{
	Serial.println();
	Serial.println("============================================================");
	Serial.print("[SESSION] STATE: ");
	Serial.println(autoStateName(autonomousState));

	Serial.print("[SESSION] Autonomous armed: ");
	Serial.println(autonomousTestArmed ? "YES" : "NO");

	Serial.print("[SESSION] Host session held: ");
	Serial.println(hostSessionHeld ? "YES" : "NO");

	Serial.print("[SESSION] Lease timeout: ");
	Serial.print(AUTONOMOUS_HOST_LEASE_MS);
	Serial.println(" ms");

	if (hostSessionHeld)
	{
		Serial.print("[SESSION] Lease remaining: ");
		Serial.print(hostLeaseRemainingMs());
		Serial.println(" ms");

		Serial.print("[SESSION] Since last HOLD/KEEPALIVE: ");
		Serial.print(millis() - hostLeaseRenewedMs);
		Serial.println(" ms");

		Serial.print("[SESSION] Session age: ");
		Serial.print((millis() - hostSessionStartedMs) / 1000UL);
		Serial.println(" s");

		Serial.print("[SESSION] Keepalives received: ");
		Serial.println(hostKeepaliveCount);

		Serial.print("[SESSION] Tethered interval open for: ");
		Serial.print((millis() - intervalStartMs) / 1000.0, 3);
		Serial.println(" s");

		Serial.print("[SESSION] Interval baseline clean: ");
		Serial.println(hostSessionBaselineEstablished ? "YES" : "NO");

		if (!hostLeaseValid())
		{
			Serial.println(
					"[SESSION] WARNING: the lease is held but already past its "
					"deadline. The next loop pass will end the session.");
		}
	}
	else
	{
		Serial.print("[SESSION] Lease remaining: 0 ms (lease length is ");
		Serial.print(AUTONOMOUS_HOST_LEASE_MS);
		Serial.println(" ms once held)");
	}

	printActiveTransports();
	Serial.print("[CONNECTION] Any host connected: ");
	Serial.println(anyHostConnected() ? "YES" : "NO");

	Serial.print("[SESSION] Rendezvous active: ");
	Serial.println(autonomousState == AUTO_STATE_USB_RENDEZVOUS ? "YES" : "NO");

	Serial.print("[SESSION] Rendezvous window: ");
	Serial.print(AUTONOMOUS_USB_RENDEZVOUS_MS);
	Serial.println(" ms per successful timer wake");

	Serial.print("[SESSION] Autonomous cadence: ");
	Serial.print(autonomousIntervalSeconds);
	Serial.println(" seconds");

	Serial.print("[SESSION] Interval accounting: ");

	if (intervalAccountingSuspended())
	{
		Serial.print("SUSPENDED (");
		Serial.print(intervalAccountingSuspendReason());
		Serial.println(")");
	}
	else
	{
		Serial.println("ACTIVE");
	}

	// Printed as diagnostics only. Nothing depends on these; see the audit in
	// LAB_NOTES 2026-09-11.
	printUsbPresence("right now");

	Serial.println("============================================================");
}

// Autonomous mode graduated from a bench test to the canonical operating mode
// on 2026-09-11. The LOGGER TEST * spelling still works so existing scripts and
// written procedures keep running, but it says so every time rather than
// quietly accepting a name that is no longer the real one.
void printDeprecatedCommand(const char *oldName, const char *newName)
{
	Serial.println();
	Serial.print("[WARNING] ");
	Serial.print(oldName);
	Serial.println(" is deprecated.");
	Serial.print("[INFO] Use ");
	Serial.print(newName);
	Serial.println(".");
}

bool armAutonomousTest()
{
	if (armedPowerTestMode() != POWER_TEST_NONE)
	{
		Serial.println(
				"[AUTO] ERROR: A power test is already armed. Only one test may run.");
		Serial.println("[AUTO] Send POWER TEST STOP first.");
		return false;
	}

	// REFUSE while a host session is held.
	//
	// Arming ends in deep sleep, and this path has no way to get there safely: it
	// resets the INA228 accumulators and sleeps, so the tethered interval that is
	// open right now would never be closed. Its charge would be discarded with no
	// CSV_DATA row, and the host's lease would vanish without a RELEASE or an
	// expiry notice.
	//
	// Every other route out of a host session goes through
	// beginAutonomousSleepFromHostSession(), which closes that interval at its
	// real elapsed length and refuses to sleep if it cannot (D-026). Refusing
	// here keeps that the only route rather than adding a second one.
	//
	// Reachable exactly as D-031 describes: hold a session on an armed board,
	// send LOGGER AUTONOMOUS OFF - which deliberately keeps the session - then
	// send LOGGER AUTONOMOUS ON.
	if (hostSessionHeld)
	{
		Serial.println(
				"[AUTO] ERROR: A host session is held, so arming would deep sleep with "
				"an open tethered interval and silently discard its charge.");
		Serial.println(
				"[AUTO] Send LOGGER SESSION RELEASE first, then LOGGER AUTONOMOUS ON.");
		Serial.println("[AUTO] No change was made. The session is untouched.");
		return false;
	}

	if (autonomousTestArmed)
	{
		// Idempotent success: the requested end state already holds. Nothing was
		// changed and nothing failed.
		Serial.println(
				"[AUTO] Autonomous bench test is already armed. No change made.");
		return true;
	}

	if (!autoStorageMount(true))
	{
		Serial.println(
				"[AUTO] ERROR: Storage is not available. The test was NOT armed, "
				"because it would have nowhere to write records.");
		return false;
	}

	if (!saveAutonomousTestArmed(true))
	{
		Serial.println(
				"[AUTO] ERROR: Could not persist the armed flag. The test was NOT "
				"armed, because it would not survive the USB disconnect.");
		return false;
	}

	autonomousTestArmed = true;
	autonomousTestRunning = true;

	rtcAutoMagic = AUTO_RTC_MAGIC;
	rtcAutoBootId = nextBootId();
	rtcAutoSessionElapsedMs = millis();
	rtcAutoCycleCount = 0;

	rtcAutoNextSeq = autoStorageRecover();
	ensureSequenceReservation(rtcAutoNextSeq, true);

	Serial.println();
	Serial.println("[AUTO] Autonomous bench test ARMED");
	Serial.print("[AUTO] Interval: ");
	Serial.print(autonomousIntervalSeconds);
	Serial.println(" seconds");
	Serial.print("[AUTO] Boot id: ");
	Serial.println(rtcAutoBootId);
	Serial.print("[AUTO] Next sequence: ");
	Serial.println(rtcAutoNextSeq);
	Serial.println(
			"[AUTO] Experiment accounting is ISOLATED: this test never modifies "
			"experiment totals or the NVS checkpoint.");
	Serial.println(
			"[AUTO] Interval accounting and CSV output are suspended while it runs.");
	Serial.println();
	Serial.println("[AUTO] HOW TO STOP IT LATER:");
	Serial.print(
			"[AUTO] Timer wakes are too short to catch. Reset or power-cycle the "
			"board, then send LOGGER TEST STOP within the ");
	Serial.print(AUTONOMOUS_COLD_BOOT_MAINTENANCE_MS / 1000UL);
	Serial.println("-second cold-boot maintenance window.");

	disableWifi();

	// Start a clean first interval.
	if (!resetInaAccumulators())
	{
		Serial.println(
				"[AUTO] ERROR: Could not start a clean first interval. Stopping.");

		autonomousTestRunning = false;

		// RAM follows NVS. Clearing the RAM flag while the NVS write fails would
		// leave the board reporting itself disarmed and resuming autonomous sleep
		// on the next boot, which is the worst of both answers.
		if (saveAutonomousTestArmed(false))
		{
			autonomousTestArmed = false;
			Serial.println("[AUTO] Persisted autonomous mode flag cleared.");
		}
		else
		{
			Serial.println(
					"[AUTO] ERROR: The persisted armed flag could NOT be cleared either. "
					"The board is still ARMED in NVS and WILL resume autonomous sleep on "
					"the next boot. Send LOGGER AUTONOMOUS OFF inside the cold-boot "
					"maintenance window.");
		}

		return false;
	}

	rtcAutoIntervalStartMs = millis();

	// The tethered interval clock follows the reset too, so that if a stop
	// cancels the pending sleep below, ordinary accounting resumes from the
	// moment the accumulators were actually cleared rather than from an older
	// start that no longer matches them (D-044).
	intervalStartMs = millis();

	// Deferred, not immediate.
	//
	// This used to call autonomousDeepSleepAgain() directly, which does not
	// return, so CMD_RESULT for LOGGER AUTONOMOUS ON was never emitted on the
	// path where the command SUCCEEDED. The host saw CMD_ACK, no result, and
	// reported a failure for a board that had armed correctly.
	//
	// Arming destroys the USB transport exactly the way RELEASE does, so it uses
	// the same bounded grace and the same mechanism (D-035). The sleep happens
	// from loop(), after processCommand() has emitted the result.
	armPendingAutonomousSleep(AUTO_SLEEP_ARM_COMMAND, "LOGGER AUTONOMOUS ON");

	return true;
}

bool stopAutonomousTest()
{
	bool wasActive = autonomousTestArmed || autonomousTestRunning;

	autonomousStopRequested = true;
	autonomousTestRunning = false;

	// A stop request supersedes a pending deferred sleep at once - before the
	// NVS write below, not after it. servicePendingAutonomousSleep() cancels only
	// once the armed flag is clear, so a failed write used to let the grace
	// expire into deep sleep after the operator had explicitly asked for a stop:
	// the D-021 failure, reached through the deferred path.
	cancelPendingAutonomousSleep("LOGGER AUTONOMOUS OFF requested");

	if (autonomousTestArmed)
	{
		if (saveAutonomousTestArmed(false))
		{
			autonomousTestArmed = false;
			Serial.println("[AUTO] Persisted autonomous mode flag cleared.");
		}
		else
		{
			// The failure this whole stint exists to stop reporting as success.
			// autonomousStopRequested stays set, so the cold-boot maintenance window
			// still refuses to sleep after an explicit stop request (D-021).
			Serial.println(
					"[AUTO] ERROR: Could not clear the persisted flag. The test will "
					"still be armed after the next reboot.");
			Serial.println(
					"[AUTO] NOT ALL OK - autonomous mode is NOT off. Send LOGGER "
					"AUTONOMOUS OFF again.");
			return false;
		}
	}

	rtcAutoMagic = 0;
	hostClaimedRendezvous = false;

	if (hostSessionHeld)
	{
		// Deliberately KEPT. Autonomous mode is the only thing the lease was
		// gating: with it off the board stays awake either way, so dropping the
		// session here would break a connected host's keepalives and gain nothing.
		// The host learns about this from STATUS, not by its next keepalive
		// suddenly failing.
		Serial.println();
		Serial.println("[AUTO] A host session is held and is being KEPT.");
		Serial.println(
				"[AUTO] Autonomous mode is now OFF, so the lease no longer gates "
				"anything: the board stays awake either way.");
		Serial.println(
				"[AUTO] KEEPALIVE and RELEASE continue to work normally for this "
				"session.");
	}
	else
	{
		hostSessionBaselineEstablished = false;
		connectionRelease(CONNECTION_USB);
	}

	setAutoState(AUTO_STATE_IDLE, "autonomous mode disabled");

	if (wasActive)
	{
		Serial.print("[AUTO] Cycles completed this power-on: ");
		Serial.println(rtcAutoCycleCount);
		Serial.println(
				"[AUTO] Stored records are NOT deleted. Use LOGGER STORAGE INFO to "
				"inspect them.");
		Serial.println("[AUTO] Autonomous mode DISABLED: ALL OK");
	}
	else
	{
		// Idempotent success, like arming an already-armed board: the requested
		// end state holds and nothing failed.
		Serial.println("[AUTO] Autonomous mode was already off.");
	}

	return true;
}

void printAutonomousStatus()
{
	Serial.println();
	Serial.print("[AUTO] Autonomous mode: ");
	Serial.println(autonomousTestArmed ? "ON" : "OFF");
	Serial.print("[AUTO] Armed: ");
	Serial.println(autonomousTestArmed ? "YES" : "NO");

	Serial.print("[AUTO] Running: ");
	Serial.println(autonomousTestRunning ? "YES" : "NO");

	Serial.print("[AUTO] Interval: ");
	Serial.print(autonomousIntervalSeconds);
	Serial.println(" seconds");

	Serial.print("[AUTO] Boot id: ");
	Serial.println(rtcAutoBootId);

	Serial.print("[AUTO] RTC state valid: ");
	Serial.println(rtcAutoMagic == AUTO_RTC_MAGIC ? "YES" : "NO");

	Serial.print("[AUTO] Cycles this power-on: ");
	Serial.println(rtcAutoCycleCount);

	Serial.print("[AUTO] Next sequence: ");
	Serial.println(rtcAutoNextSeq);

	Serial.print("[AUTO] Sequence reserved through: ");
	Serial.println(rtcAutoSeqHighWater);

	Serial.print("[AUTO] Session elapsed: ");
	Serial.print(rtcAutoSessionElapsedMs);
	Serial.println(" ms");

	Serial.println(
			"[AUTO] Accounting: ISOLATED. Experiment totals are never modified.");

	uint32_t statusExperimentId = 0;

	if (loadExperimentIdOnly(statusExperimentId))
	{
		Serial.print("[AUTO] New records will carry experiment id: ");
		Serial.println(statusExperimentId);
	}
	else
	{
		Serial.println(
				"[AUTO] ERROR: Experiment id cannot be determined from NVS.");
		Serial.println(
				"[AUTO] New records would be marked EXPERIMENT UNKNOWN rather than "
				"claiming experiment 0.");
	}

	Serial.print("[AUTO] Cold-boot maintenance window: ");
	Serial.print(AUTONOMOUS_COLD_BOOT_MAINTENANCE_MS / 1000UL);
	Serial.println(" seconds (cold boot only; timer wakes do not open one)");

	printUsbPresence("right now");
}

// ============================================================================
// AUTONOMOUS BENCH TEST: STORAGE COMMANDS
// ============================================================================

bool printStorageInfo()
{
	Serial.println();

	if (!autoStorageMount(true))
	{
		return false;
	}

	AutoLogScan scan = autoStorageScan();

	Serial.print("[STORAGE] Filesystem total: ");
	Serial.print(static_cast<unsigned long>(LittleFS.totalBytes()));
	Serial.println(" bytes");

	Serial.print("[STORAGE] Filesystem used:  ");
	Serial.print(static_cast<unsigned long>(LittleFS.usedBytes()));
	Serial.println(" bytes");

	Serial.print("[STORAGE] Filesystem free:  ");
	Serial.print(static_cast<unsigned long>(
			LittleFS.totalBytes() - LittleFS.usedBytes()));
	Serial.println(" bytes");

	Serial.print("[STORAGE] Record size:      ");
	Serial.print(static_cast<unsigned long>(AUTO_RECORD_SIZE));
	Serial.println(" bytes");

	if (!scan.fileExists)
	{
		// A board that has never written a record answers this correctly.
		Serial.println("[STORAGE] Log file:         does not exist yet");
		return true;
	}

	Serial.print("[STORAGE] Log bytes:        ");
	Serial.println(static_cast<unsigned long>(scan.fileBytes));

	Serial.print("[STORAGE] Valid records:    ");
	Serial.println(scan.validRecords);

	if (scan.validRecords > 0)
	{
		Serial.print("[STORAGE] Oldest sequence:  ");
		Serial.println(scan.firstSeq);

		Serial.print("[STORAGE] Newest sequence:  ");
		Serial.println(scan.lastSeq);
	}

	Serial.print("[STORAGE] Trailing bytes:   ");
	Serial.println(static_cast<unsigned long>(scan.trailingBytes));

	Serial.print("[STORAGE] Tail status:      ");

	if (scan.trailingBytes == 0)
	{
		Serial.println("INTACT");
	}
	else if (scan.partialTail)
	{
		Serial.println("PARTIAL WRITE DETECTED");
	}
	else
	{
		Serial.println("INVALID RECORD DETECTED");
	}

	if (scan.seqOutOfOrder)
	{
		Serial.println(
				"[STORAGE] WARNING: sequence numbers are not strictly increasing.");
	}

	// Capacity projection uses the configured cadence rather than assuming one.
	uint32_t recordsPerDay = 86400UL / autonomousIntervalSeconds;
	size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
	uint32_t freeRecords = freeBytes / AUTO_RECORD_SIZE;

	Serial.print("[STORAGE] Room for ");
	Serial.print(freeRecords);
	Serial.print(" more records = ");
	Serial.print(recordsPerDay > 0 ? freeRecords / recordsPerDay : 0);
	Serial.print(" days at ");
	Serial.print(autonomousIntervalSeconds);
	Serial.println(" second cadence");

	return true;
}

bool dumpStorage()
{
	Serial.println();

	if (!autoStorageMount(true))
	{
		return false;
	}

	if (!LittleFS.exists(AUTO_LOG_PATH))
	{
		Serial.println("[STORAGE] No log file to dump.");
		return true;
	}

	File file = LittleFS.open(AUTO_LOG_PATH, FILE_READ);

	if (!file)
	{
		Serial.println("[STORAGE] ERROR: Could not open the log for dump.");
		return false;
	}

	Serial.println("[STORAGE] --- BEGIN DUMP ---");

	AutoRecord record;
	uint32_t index = 0;
	uint32_t bad = 0;

	while (file.available() >= static_cast<int>(AUTO_RECORD_SIZE))
	{
		if (file.read(reinterpret_cast<uint8_t *>(&record), AUTO_RECORD_SIZE) !=
				AUTO_RECORD_SIZE)
		{
			Serial.println("[STORAGE] ERROR: Short read during dump.");
			break;
		}

		Serial.print("[STORAGE] #");
		Serial.print(index);
		Serial.print(" ");

		if (!autoRecordValid(record))
		{
			bad++;
			Serial.print("INVALID (magic=0x");
			Serial.print(record.magic, HEX);
			Serial.print(" version=");
			Serial.print(record.version);
			Serial.print(" crc_stored=0x");
			Serial.print(record.crc32, HEX);
			Serial.print(" crc_computed=0x");
			Serial.print(autoRecordCrc(record), HEX);
			Serial.println(")");
		}
		else
		{
			printAutoRecord(record);
		}

		index++;
	}

	size_t remainder = file.size() % AUTO_RECORD_SIZE;
	file.close();

	Serial.println("[STORAGE] --- END DUMP ---");

	Serial.print("[STORAGE] Records read: ");
	Serial.print(index);
	Serial.print(", invalid: ");
	Serial.println(bad);

	if (remainder != 0)
	{
		Serial.print("[STORAGE] WARNING: ");
		Serial.print(static_cast<unsigned long>(remainder));
		Serial.println(" trailing bytes are not a whole record.");
	}

	// The dump itself succeeded. Invalid records and a ragged tail are findings
	// it is reporting, not failures of the command, and they are already counted
	// above. A short read mid-dump is different and is reported there.
	return true;
}

bool clearStorage()
{
	Serial.println();
	Serial.println("[STORAGE] Clearing autonomous test storage...");
	Serial.println(
			"[STORAGE] This does NOT touch experiment state: experiment id, "
			"interval number, and running totals are unaffected.");

	if (autonomousTestRunning || autonomousTestArmed)
	{
		Serial.println(
				"[STORAGE] ERROR: The autonomous test is armed. Stop it first with "
				"LOGGER TEST STOP.");
		return false;
	}

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

	if (!autoStorageMount(true))
	{
		Serial.println("[STORAGE] ERROR: Could not mount after format.");
		return false;
	}

	rtcAutoRunChargeUAh = 0;
	rtcAutoRunEnergyUWh = 0;

	Serial.println("[STORAGE] Storage cleared and mounted: ALL OK");
	Serial.println(
			"[STORAGE] Note: the sequence high-water mark in NVS is deliberately "
			"NOT reset, so sequence numbers are never reused after a clear.");

	return true;
}

// ============================================================================
// ARDUINO SETUP()
// ============================================================================
//
// setup() runs ONCE after boot/reset.
// ============================================================================

void setup()
{
	// First line, so every "total duration" the autonomous path reports covers
	// the whole awake period instead of starting partway through it.
	setupEntryMicros = micros();

	// Capture the wake cause before anything else can run. A deep-sleep wake
	// arrives here as an ordinary boot, so this register read is the only way
	// to tell "the user plugged it in" apart from "our sleep timer fired".
	bootWakeupCause = esp_sleep_get_wakeup_cause();

	// Sixteen times the core's 256-byte default, set before begin() so begin()
	// keeps it. The default is small enough that one verbose block overflows it,
	// and with the zero timeout below an overflow means dropped text rather than
	// a wait. A larger buffer makes that rare instead of routine.
	Serial.setTxBufferSize(4096);

	Serial.begin(115200);

	// --------------------------------------------------------------------------
	// Serial output must never block the state machine.
	// --------------------------------------------------------------------------
	//
	// ROOT CAUSE of the 2026-09-11 lease failure. HWCDC defaults to
	// tx_timeout_ms = 100 with max_consec_timeouts = 20, so a single blocked
	// Serial write can stall for about two seconds once the TX ring buffer fills
	// and nobody is draining it. That is exactly what happens when a host opens
	// the port, captures a few seconds, and closes it again: isPlugged() keeps
	// reporting true, the ring fills, and every subsequent print pays the full
	// bounded wait.
	//
	// With a hundred-plus lines of initialization output that added up to roughly
	// 74 seconds inside setup(), which is why loop() was never reached and the
	// 15-second host lease never expired.
	//
	// A zero timeout drops output instead of waiting when no one is reading.
	// That trade is deliberate: Serial output here is diagnostics, and
	// diagnostics may never be allowed to stall measurement, sleep decisions, or
	// lease expiry.
	//
	// What this costs: when the TX buffer is full, text is dropped rather than
	// queued. That only happens when nothing is draining the port, which is
	// exactly when the text has no recipient. CSV telemetry is not put at risk by
	// it either, because a connected Python logger drains continuously and the
	// buffer stays near empty; unattended capture is the durable record log's
	// job, never Serial's.
	Serial.setTxTimeoutMs(0);

	// Before the timer-wake branch below, so every boot AND every autonomous
	// wake states which image is running. Three short nonblocking lines: with
	// setTxTimeoutMs(0) in force they cannot stall the wake path, and knowing
	// what is on the board is worth more than the microseconds.
	printFirmwareIdentity();

	// --------------------------------------------------------------------------
	// AUTONOMOUS BENCH TEST: EARLY TIMER-WAKE BRANCH
	// --------------------------------------------------------------------------
	//
	// This branch runs BEFORE everything below it, and that ordering is the
	// whole point of the test.
	//
	// The normal cold-boot path further down calls configureIna228() and then
	// resetInaAccumulators() without ever reading ENERGY or CHARGE. On a
	// deep-sleep wake that would destroy the interval this test exists to
	// capture, and nothing would say so.
	//
	// It also skips the two development delays deliberately:
	//
	//   delay(1500)  USB CDC enumeration. NOTE: this is no longer because
	//                enumeration does not matter. It very much does now. The USB
	//                rendezvous that follows the measurement is ten seconds long,
	//                which subsumes this delay many times over, and putting the
	//                delay ahead of the accumulator read would only postpone the
	//                measurement. Serial.begin() has already run above, so the
	//                peripheral is initialized before the wait that matters.
	//
	//   delay(2000)  ADC settle. The INA228 stayed powered and in continuous
	//                conversion for the entire sleep, so its ADC is already
	//                settled. Nothing about the CPU waking changes that. The
	//                datasheet's 60 us start-up time applies to leaving
	//                shutdown, which is not what happened here.
	//
	// Cold-boot behavior below is unchanged.
	//
	// Timings are measured from the top of runAutonomousWakeCycle() and printed
	// after the work completes, so Serial output cannot inflate them.
	// --------------------------------------------------------------------------
	if (bootWakeupCause == ESP_SLEEP_WAKEUP_TIMER)
	{
		loadAutonomousSettings();

		if (autonomousTestArmed)
		{
			if (rtcAutoMagic != AUTO_RTC_MAGIC)
			{
				// Armed in NVS but RTC state is gone, so this is a timer wake whose
				// session context was lost. Do not guess the sequence or the running
				// totals; rebuild them from the durable log instead.
				Serial.println();
				Serial.println(
						"[AUTO] WARNING: Timer wake with no valid RTC session state.");
				Serial.println(
						"[AUTO] Rebuilding sequence and totals from the durable log.");

				if (!autoStorageMount(true))
				{
					Serial.println(
							"[AUTO] ERROR: Storage unavailable. Falling through to normal "
							"cold boot rather than logging into nothing.");
				}
				else
				{
					rtcAutoMagic = AUTO_RTC_MAGIC;
					rtcAutoBootId = nextBootId();
					rtcAutoSessionElapsedMs = 0;
					rtcAutoIntervalStartMs = 0;
					rtcAutoCycleCount = 0;
					rtcAutoNextSeq = autoStorageRecover();
					ensureSequenceReservation(rtcAutoNextSeq, true);

					autonomousTestRunning = true;

					// The elapsed interval cannot be attributed to a known duration,
					// so discard it rather than record a wrong one, and start clean.
					Serial.println(
							"[AUTO] Discarding the elapsed interval: its duration is not "
							"known after RTC state loss.");
					resetInaAccumulators();

					rtcAutoIntervalStartMs = millis();

					autonomousDeepSleepAgain(AUTO_SLEEP_RTC_LOST);
					return;
				}
			}
			else
			{
				autonomousTestRunning = true;

				if (!runAutonomousWakeCycle())
				{
					// Not reached in practice: the cycle ends in deep sleep.
					return;
				}

				// A host claimed the rendezvous, or the test was disarmed during it.
				// Fall through into the ordinary initialization below so the board
				// comes up as a normal tethered logger for this awake period.
				Serial.println();
				Serial.println(
						"[AUTO] Staying awake. Continuing into normal initialization so "
						"this wake can serve a host session.");
				Serial.println(
						"[AUTO] NOTE: charge accumulated during the rendezvous belongs to "
						"no interval. The initialization below resets the accumulators to "
						"start a clean tethered interval, so that window is a stated gap "
						"rather than a silent loss.");
			}
		}
	}

	// Give USB Serial time to enumerate before printing the boot sequence.
	//
	// After a deep-sleep wake the USB device has to re-enumerate on the host,
	// so this delay is doing real work on every wake, not just at power-on.
	if (anyHostConnected())
	{
		Serial.println(
				"[LOGGER] Skipping the 1500 ms USB enumeration delay: a host is "
				"already talking to us, so there is nothing to wait for.");
	}
	else
	{
		delay(1500);
	}

	printBootWakeReason();

	initializeWifiOff();

	Serial.println();

	Serial.println(
			"============================================================");

	Serial.println(
			"BMW SOLAR LOGGER - DEVELOPMENT FIRMWARE");

	Serial.println(
			"INA228 + XIAO ESP32-C3");

	Serial.println(
			"NVS schema 2 + experiment-aware CSV");

	Serial.println(
			"============================================================");

	Serial.println();

	intervalStartMs = millis();
	lastHeartbeatMs = intervalStartMs;
	// Start the live-sample clock from the same point.
	lastLiveSampleMs = intervalStartMs;

	// --------------------------------------------------------------------------
	// Start I2C.
	// --------------------------------------------------------------------------

	Serial.println("[BOOT] Starting I2C...");

	// XIAO ESP32-C3:
	//
	//   D4 -> INA228 SDA
	//   D5 -> INA228 SCL
	//
	// SDA = Serial Data
	// SCL = Serial Clock
	Wire.begin(D4, D5);

	// 400 kHz is I2C "Fast Mode".
	Wire.setClock(400000);

	Serial.println(
			"[BOOT] I2C started: SDA=D4, SCL=D5, 400 kHz");

	// --------------------------------------------------------------------------
	// Make sure the device at 0x40 is actually our INA228.
	// --------------------------------------------------------------------------

	if (!verifyInaIdentity())
	{
		Serial.println(
				"[FATAL] INA228 identity check failed. Logger halted.");

		while (true)
		{
			Serial.println(
					"[FATAL] NOT ALL OK - fix INA228/I2C wiring and reset.");

			delay(5000);
		}
	}

	// --------------------------------------------------------------------------
	// Apply known-good INA228 configuration and READ IT BACK.
	// --------------------------------------------------------------------------

	if (!configureIna228())
	{
		Serial.println(
				"[FATAL] INA228 configuration failed. Logger halted.");

		while (true)
		{
			Serial.println(
					"[FATAL] NOT ALL OK - configuration failure.");

			delay(5000);
		}
	}

	// --------------------------------------------------------------------------
	// Restore experiment state from flash.
	// --------------------------------------------------------------------------

	Serial.println();

	Serial.println(
			"[BOOT] Loading persistent state from NVS...");

	if (!loadCheckpoint())
	{
		Serial.println(
				"[FATAL] NVS load failed. Logger halted to avoid corrupting totals.");

		while (true)
		{
			Serial.println(
					"[FATAL] NOT ALL OK - NVS state requires attention.");

			delay(5000);
		}
	}

	// The flag must survive a USB disconnect and reboot before battery power is
	// applied, so load it independently from the experiment checkpoint.
	if (!loadWifiPowerTestArmed())
	{
		Serial.println(
				"[FATAL] Wi-Fi power-test state could not be loaded. Logger halted.");

		while (true)
		{
			Serial.println(
					"[FATAL] NOT ALL OK - Wi-Fi power-test NVS state requires attention.");

			delay(5000);
		}
	}

	// Same reasoning for the deep-sleep test: the whole point is that arming it
	// over USB survives the USB disconnect and the switch to AA battery power.
	if (!loadSleepPowerTestArmed())
	{
		Serial.println(
				"[FATAL] Deep-sleep test state could not be loaded. Logger halted.");

		while (true)
		{
			Serial.println(
					"[FATAL] NOT ALL OK - deep-sleep test NVS state requires attention.");

			delay(5000);
		}
	}

	// Autonomous bench-test settings. Loaded on every boot so STATUS reports
	// the real cadence rather than the compile-time default.
	loadAutonomousSettings();

	// The variant matters as much as the armed flag. Without this load, an
	// armed INA-OFF test would silently resume as the INA-continuous variant
	// after every power cycle, and STATUS would report the wrong one while the
	// DMM measured something else entirely.
	if (!loadSleepPowerTestInaOff())
	{
		Serial.println(
				"[FATAL] Deep-sleep test variant could not be loaded. Logger halted.");

		while (true)
		{
			Serial.println(
					"[FATAL] NOT ALL OK - deep-sleep variant NVS state requires "
					"attention.");

			delay(5000);
		}
	}

	// --------------------------------------------------------------------------
	// Let the ADC settle.
	// --------------------------------------------------------------------------

	Serial.println();

	if (anyHostConnected())
	{
		Serial.println(
				"[INA228] Skipping the 2000 ms ADC settle: the INA228 has been "
				"converting continuously and a host session is waiting on its lease.");
	}
	else
	{
		Serial.println(
				"[INA228] Waiting 2 seconds for ADC measurements to settle...");

		delay(2000);
	}

	Serial.println(
			"[INA228] ADC settle complete.");

	// --------------------------------------------------------------------------
	// Take one initial live sensor reading.
	// --------------------------------------------------------------------------

	Serial.println();

	Serial.println(
			"[INA228] Reading current sensor state...");

	SensorReading reading;

	if (!readSensor(reading, true))
	{
		Serial.println(
				"[FATAL] Initial INA228 measurement failed. Logger halted.");

		while (true)
		{
			Serial.println(
					"[FATAL] NOT ALL OK - initial measurement failure.");

			delay(5000);
		}
	}

	Serial.print("[INA228] Voltage      = ");
	Serial.print(reading.voltage_V, 4);
	Serial.println(" V");

	Serial.print("[INA228] Current      = ");
	Serial.print(reading.current_mA, 3);
	Serial.println(" mA");

	Serial.print("[INA228] Power        = ");
	Serial.print(reading.power_mW, 3);
	Serial.println(" mW");

	Serial.print("[INA228] Temperature  = ");
	Serial.print(reading.temperature_C, 2);
	Serial.println(" C");

	// --------------------------------------------------------------------------
	// Start a clean hardware accumulation interval.
	//
	// Note:
	// This does NOT reset the experiment totals stored in NVS.
	//
	// It only clears the INA228's short-term hardware ENERGY/CHARGE counters so
	// the first interval after boot starts from zero.
	// --------------------------------------------------------------------------

	Serial.println();

	if (hostSessionBaselineEstablished)
	{
		// A host claimed the rendezvous and LOGGER SESSION HOLD already reset the
		// accumulators and started the interval. Resetting again here would throw
		// away everything measured since the claim, which is exactly the bug this
		// guard exists to prevent.
		Serial.println();
		Serial.println(
				"[LOGGER] Measurement interval was already started by LOGGER SESSION "
				"HOLD.");
		Serial.print("[LOGGER] Interval open for ");
		Serial.print((millis() - intervalStartMs) / 1000.0, 3);
		Serial.println(" s so far. NOT resetting the accumulators again.");
	}
	else
	{
		Serial.println();

		Serial.println(
				"[LOGGER] Preparing measurement interval...");

		if (!resetInaAccumulators())
		{
			Serial.println(
					"[FATAL] Could not initialize accumulation interval. Logger halted.");

			while (true)
			{
				Serial.println(
						"[FATAL] NOT ALL OK - accumulator reset failure.");

				delay(5000);
			}
		}

		intervalStartMs = millis();

		lastHeartbeatMs = intervalStartMs;
		lastLiveSampleMs = intervalStartMs;

		Serial.println(
				"[LOGGER] Measurement interval started.");
	}

	// --------------------------------------------------------------------------
	// Boot summary.
	// --------------------------------------------------------------------------

	Serial.println();

	Serial.println(
			"============================================================");

	Serial.println("ALL OK");

	Serial.println(
			"Initialization completed successfully.");

	Serial.println();

	Serial.println(
			"Measurement interval: 60 seconds");

	Serial.println(
			"Live sample interval: 1 second");

	Serial.println(
			"Heartbeat interval:   5 seconds");

	Serial.print(
			"NVS checkpoint every: ");

	Serial.print(
			NVS_CHECKPOINT_EVERY_INTERVALS);

	Serial.println(
			" interval");

	Serial.print(
			"Shunt resistance:     ");

	Serial.print(
			SHUNT_OHMS * 1000.0,
			4);

	Serial.println(
			" mOhm");

	Serial.print(
			"VSHUNT LSB:           ");

	Serial.print(
			VSHUNT_LSB_V * 1.0e9,
			1);

	Serial.println(
			" nV/count");

	Serial.println();

	Serial.print(
			"Restored experiment:  ");

	Serial.println(
			experimentId);

	Serial.print(
			"Restored interval:    ");

	Serial.println(
			completedInterval);

	Serial.print(
			"Restored charge:      ");

	Serial.print(
			runningCharge_mAh,
			6);

	Serial.println(
			" mAh");

	Serial.print(
			"Restored energy:      ");

	Serial.print(
			runningEnergy_mWh,
			6);

	Serial.println(
			" mWh");

	Serial.println(
			"============================================================");

	// This tells a CSV parser:
	//
	//   "The device rebooted, but this is still the same experiment."
	printExperimentResumeEvent();

	printCsvHeader();

	printHelp();

	// --------------------------------------------------------------------------
	// Resume a persisted power test, if one is armed.
	// --------------------------------------------------------------------------
	//
	// Exactly one test may run. armedPowerTestMode() is the single place that
	// decides which, so a boot can never start both.

	// The autonomous bench test resumes after a cold boot, exactly like the
	// power tests, so it survives the USB disconnect and the switch to battery
	// power. This runs before the power-test dispatch because the two are
	// mutually exclusive and this one owns deep sleep.
	if (autonomousTestArmed && anyHostConnected())
	{
		// Reached when a timer wake was claimed during its rendezvous and fell
		// through to here. Opening a maintenance window and sleeping would throw
		// away the session the host just took.
		Serial.println();
		Serial.println(
				"[AUTO] A host session is held, so the autonomous resume below is "
				"skipped and the board stays awake.");
		Serial.print("[AUTO] Lease remaining: ");
		Serial.print(hostLeaseRemainingMs());
		Serial.println(" ms");
		Serial.println(
				"[AUTO] Autonomous mode remains ARMED and resumes when the host "
				"releases or stops renewing.");
	}
	else if (autonomousTestArmed)
	{
		Serial.println();
		Serial.println(
				"[AUTO] Persisted autonomous bench test found after a cold boot.");
		Serial.print("[AUTO] Interval: ");
		Serial.print(autonomousIntervalSeconds);
		Serial.println(" seconds");

		if (bootWakeupCause == ESP_SLEEP_WAKEUP_TIMER)
		{
			Serial.println(
					"[AUTO] NOTE: This is a timer wake that fell through to the "
					"cold-boot path, which only happens after an earlier error. The "
					"maintenance window below is therefore opening on a wake.");
		}

		if (!autoStorageMount(true))
		{
			Serial.println(
					"[AUTO] ERROR: Storage unavailable. The test is armed but cannot "
					"run, so it is being stopped rather than looping without storing.");

			if (!stopAutonomousTest())
			{
				// The board is about to continue as an ordinary tethered logger while
				// NVS still says armed, so the next boot will arrive here again and
				// find the same missing storage. Say that, rather than leaving a loop
				// whose cause is only visible by comparing two boots.
				Serial.println(
						"[AUTO] ERROR: The armed flag could NOT be cleared, so this boot "
						"will repeat on the next reset. Fix storage, or clear the flag "
						"with LOGGER AUTONOMOUS OFF once NVS is writable.");
			}
		}
		else if (!autonomousColdBootMaintenanceWindow())
		{
			// Stopped by the operator during the window. Fall through to normal
			// logging; the persisted flag is already cleared.
		}
		else
		{
			rtcAutoMagic = AUTO_RTC_MAGIC;
			rtcAutoBootId = nextBootId();
			rtcAutoCycleCount = 0;

			const uint32_t tScanStart = micros();
			rtcAutoNextSeq = autoStorageRecover();
			const uint32_t tScanDone = micros();

			ensureSequenceReservation(rtcAutoNextSeq, true);

			autonomousTestRunning = true;

			Serial.print("[AUTO] Boot id: ");
			Serial.println(rtcAutoBootId);
			Serial.print("[AUTO] Next sequence: ");
			Serial.println(rtcAutoNextSeq);

			Serial.print("[TIMING] Storage scan/recovery:  ");
			Serial.print((tScanDone - tScanStart) / 1000.0, 3);
			Serial.println(" ms (cold boot only; a timer wake does not scan)");

			// Start the first interval here rather than inheriting whatever the
			// cold-boot initialization left in the accumulators. The charge measured
			// during initialization and the maintenance window belongs to no record
			// and is deliberately discarded, which is a stated gap rather than a
			// silently mis-attributed interval.
			Serial.println(
					"[AUTO] Discarding charge accumulated during startup and the "
					"maintenance window, so the first interval is exactly one cadence "
					"long.");

			if (!resetInaAccumulators())
			{
				Serial.println(
						"[AUTO] ERROR: Could not start a clean first interval. Stopping "
						"rather than sleeping with unknown accumulator contents.");

				if (!stopAutonomousTest())
				{
					Serial.println(
							"[AUTO] ERROR: The armed flag could NOT be cleared, so this boot "
							"will repeat on the next reset. The board stays awake for "
							"diagnosis instead of sleeping with an unknown accumulator "
							"state.");
				}
			}
			else
			{
				rtcAutoSessionElapsedMs = millis();
				rtcAutoIntervalStartMs = rtcAutoSessionElapsedMs;

				autonomousDeepSleepAgain(AUTO_SLEEP_COLD_BOOT);
				return;
			}
		}
	}

	PowerTestMode mode = armedPowerTestMode();

	if (mode == POWER_TEST_CONFLICT)
	{
		// Never guess which test the user meant. Refuse both, keep logging
		// normally, and say exactly how to fix it.
		Serial.println();
		Serial.println(
				"[POWER TEST] ERROR: Both the Wi-Fi test flag and the deep-sleep "
				"test flag are armed in NVS.");
		Serial.println(
				"[POWER TEST] Only one power test may run, so NEITHER was started.");
		Serial.println(
				"[POWER TEST] Send POWER TEST STOP to clear both flags.");
		Serial.println(
				"[POWER TEST] Normal continuous logging continues in the meantime.");
	}
	else if (mode == POWER_TEST_WIFI)
	{
		wifiPowerTestRunning = true;
		Serial.println("[POWER TEST] Persisted Wi-Fi power test found.");
		Serial.println("[POWER TEST] Wi-Fi power test ARMED");
		startWifiPowerTestPhase(1);
	}
	else if (mode == POWER_TEST_SLEEP)
	{
		Serial.println();
		Serial.println("[POWER TEST] Persisted deep-sleep power test found.");

		bool rtcStateValid = (rtcSleepTestMagic == SLEEP_POWER_TEST_RTC_MAGIC);

		if (bootWakeupCause == ESP_SLEEP_WAKEUP_TIMER)
		{

			if (rtcStateValid)
			{
				if (sleepPowerTestInaOff)
				{
					Serial.println(
							"[POWER TEST] Woke from INA-OFF deep sleep: ALL OK");
				}
				else
				{
					Serial.println("[POWER TEST] Woke from deep sleep timer: ALL OK");
				}

				Serial.println("[POWER TEST] Starting next AWAKE phase.");
			}
			else
			{
				// Woke from our timer, but RTC memory does not hold our marker. The
				// cycle count is genuinely unknown, so say that instead of printing
				// whatever number happened to be in RAM.
				Serial.println("[POWER TEST] Woke from deep sleep timer: ALL OK");
				Serial.println(
						"[POWER TEST] WARNING: RTC-retained test state was lost. The "
						"cycle counter is restarting from 0.");
				Serial.println(
						"[POWER TEST] Sleep-current measurements are unaffected; only "
						"the cycle count is.");

				rtcSleepTestMagic = SLEEP_POWER_TEST_RTC_MAGIC;
				rtcSleepTestCycle = 0;
			}

			printSleepTestEvent("SLEEP_TEST_WAKE", rtcSleepTestCycle);
		}
		else
		{
			// Power-on or reset. This is the expected path when the user unplugs
			// USB and applies AA battery power, which is the whole reason the
			// armed flag lives in NVS.
			Serial.print("[POWER TEST] This boot was not a deep-sleep wake (");
			Serial.print(wakeupCauseName(bootWakeupCause));
			Serial.println("), so the test is starting a fresh cycle count.");

			rtcSleepTestMagic = SLEEP_POWER_TEST_RTC_MAGIC;
			rtcSleepTestCycle = 0;

			printSleepTestEvent("SLEEP_TEST_RESUMED", rtcSleepTestCycle);
		}

		// setup() has already run configureIna228(), which writes and verifies
		// CONFIG, ADC_CONFIG, and SHUNT_CAL. For the INA-OFF variant, confirm
		// from the device that continuous conversion is genuinely back before
		// saying so, rather than assuming the boot path worked.
		if (sleepPowerTestInaOff)
		{
			if (inaIsInContinuousMode(true))
			{
				inaShutdownActive = false;
				Serial.println("[INA228] Continuous measurement restored: ALL OK");
			}
			else
			{
				Serial.println(
						"[INA228] ERROR: INA228 does not report continuous mode after "
						"wake, even though boot configuration reported success.");

				if (!restoreInaContinuousMode())
				{
					Serial.println(
							"[POWER TEST] ERROR: Stopping the sleep test. Continuing would "
							"measure an INA228 in an unknown state.");
					stopAllPowerTests();
					return;
				}

				inaShutdownActive = false;
			}
		}

		startSleepPowerTestAwakePhase();
	}
}

// ============================================================================
// ARDUINO LOOP()
// ============================================================================
//
// loop() runs repeatedly for as long as the ESP32 is awake.
//
// There is no operating-system task scheduler here.
//
// We repeatedly ask:
//
//   Is there Serial input?
//   Is heartbeat time here?
//   Has the 60-second interval expired?
// ============================================================================

// Expire a host lease on real elapsed monotonic time.
//
// RELEASE is an optimization, not the recovery path. A crashed logger, a pulled
// cable, or a laptop suspending all stop the keepalives without sending
// anything, and this is what makes those cases resolve on their own. No client
// can leave the board permanently awake by dying.
//
// Called from loop(). On 2026-09-11 that was the ONLY caller and loop() was
// never reached, because blocking Serial writes held setup() for ~74 s. The
// blocking is fixed at its source with setTxTimeoutMs(0); this function is kept
// separate so the check is easy to service from anywhere that could ever run
// long.
void serviceHostLease()
{
	if (!hostSessionHeld || hostLeaseValid())
	{
		return;
	}

	const uint32_t quietMs = millis() - hostLeaseRenewedMs;

	Serial.println();
	Serial.print("[SESSION] Host lease EXPIRED after ");
	Serial.print(quietMs);
	Serial.println(" ms without keepalive.");

	Serial.print("[SESSION] Lease timeout is ");
	Serial.print(AUTONOMOUS_HOST_LEASE_MS);
	Serial.print(" ms. Keepalives received this session: ");
	Serial.println(hostKeepaliveCount);

	Serial.print("[SESSION] Session lasted ");
	Serial.print((millis() - hostSessionStartedMs) / 1000.0, 3);
	Serial.println(" s.");

	hostSessionHeld = false;
	hostClaimedRendezvous = false;
	hostSessionBaselineEstablished = false;

	Serial.println(
			"[CONNECTION] USB transport RELEASED due to lease expiry.");
	connectionRelease(CONNECTION_USB);

	setAutoState(AUTO_STATE_HOST_LEASE_EXPIRED, "no KEEPALIVE before deadline");

	if (autonomousTestArmed)
	{
		Serial.println("[AUTO] Returning to autonomous sleep.");

		// Does not return when it succeeds. There is no command to answer here, so
		// a failed handoff is reported to the console only.
		const HandoffResult handoff =
				beginAutonomousSleepFromHostSession("host lease expired", true);

		if (handoff != HANDOFF_PREPARED)
		{
			Serial.print(
					"[SESSION] NOT ALL OK - lease expiry could not hand back to "
					"autonomous sleep. Failed stage: ");
			Serial.println(handoffStageName(handoff));
		}
	}
	else
	{
		Serial.println(
				"[AUTO] Autonomous mode is not armed, so the board simply continues "
				"logging normally.");
		setAutoState(AUTO_STATE_IDLE, "lease expired, autonomous not armed");
	}
}

void loop()
{
	// Process incoming user commands without waiting/blocking.
	handleSerialCommands();

	// --------------------------------------------------------------------------
	// HOST SESSION LEASE
	// --------------------------------------------------------------------------
	//
	serviceHostLease();

	// RELEASE has already completed its accounting and emitted its explicit
	// acknowledgement. Sleep only after its short, bounded grace period.
	servicePendingAutonomousSleep();

	unsigned long now =
			millis();

	// Keep the power test responsive without delay(30000), so Serial commands,
	// INA228 measurements, heartbeat output, CSV output, and NVS accounting
	// continue running normally during every Wi-Fi phase.
	updateWifiPowerTest(now);

	// The deep-sleep test's awake phase ends here. On success this call does
	// not return: the chip powers down and the next code to run is setup().
	updateSleepPowerTest(now);

	// --------------------------------------------------------------------------
	// 1-SECOND LIVE TELEMETRY
	// --------------------------------------------------------------------------
	//
	// This is intentionally independent of the 60-second accumulation interval.
	//
	// Reading VBUS/CURRENT/POWER does not reset or otherwise disturb the INA228
	// CHARGE and ENERGY accumulators.
	//
	if (
			now - lastLiveSampleMs >=
			LIVE_SAMPLE_INTERVAL_MS)
	{

		// Advance based on the current time.
		//
		// For this development logger, exact sub-millisecond scheduling is not
		// important. We care about approximately one sample each second.
		lastLiveSampleMs = now;

		printLiveSample();
	}

	// --------------------------------------------------------------------------
	// HEARTBEAT
	// --------------------------------------------------------------------------

	if (
			now - lastHeartbeatMs >=
			HEARTBEAT_INTERVAL_MS)
	{

		lastHeartbeatMs = now;

		printHeartbeat();
	}

	// --------------------------------------------------------------------------
	// 60-SECOND MEASUREMENT
	// --------------------------------------------------------------------------

	// Interval accounting is explicitly gated rather than left to arithmetic.
	//
	// During the deep-sleep test the 60-second timer would never fire anyway,
	// because millis() restarts on every wake and the awake phase is only 30
	// seconds. Relying on that accident would mean the logger was silently not
	// accounting while nothing said so. The suspension is announced when the
	// awake phase starts and reported by STATUS and POWER TEST STATUS.
	if (
			!intervalAccountingSuspended() &&
			now - intervalStartMs >=
					MEASUREMENT_INTERVAL_MS)
	{

		if (!closeMeasurementInterval())
		{
			Serial.println(
					"[ERROR] Interval close failed.");

			Serial.println(
					"[ERROR] Keeping logger awake for diagnosis.");

			// Do NOT silently pretend a clean new interval began.
			delay(1000);
		}
	}
}