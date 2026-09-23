// ============================================================================
// NVS PERSISTENCE - implementation
// ============================================================================
//
// Moved out of solar-logger.ino with no intended behavior change. The bodies
// are the ones that were there, with two mechanical differences forced by the
// move and nothing else:
//
//   - a function that used to write a sketch global now writes an out
//     parameter, because a module may not reach into its caller's state
//   - the namespace, the key names and the Preferences object are `static`
//     here, because nothing outside this file uses them (D-047)
//
// Two functions were SPLIT rather than moved whole, because each mixed NVS
// mechanics with a policy decision that belongs to the caller:
//
//   loadAutonomousSettings()  the read moved here as loadAutonomousConfig();
//                             the cadence range check stayed in the sketch
//                             with the cadence constants it tests (D-018)
//   reserveSequenceBlock()    the write moved here as saveSequenceHighWater();
//                             the block size and the RTC mirror stayed in the
//                             sketch, which owns both (D-023)
//
// See nvs_persistence.h for the boundary and for the two preserved defects.
//
// A .cpp is compiled on its own, without the #include <Arduino.h> that Arduino
// CLI prepends to the sketch, so it includes what it uses.
// ============================================================================

#include "nvs_persistence.h"

#include <Arduino.h>
#include <Preferences.h>

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

static Preferences preferences;

static constexpr char NVS_NAMESPACE[] = "solarlog";

// ESP32 NVS key names are limited to 15 usable characters. "wifi_test_armed"
// is exactly 15. "sleep_test_armed" would be 16 and would be rejected, so the
// deep-sleep test flag uses the shorter "sleep_test_arm".
static constexpr char NVS_WIFI_POWER_TEST_KEY[] = "wifi_test_armed";
static constexpr char NVS_SLEEP_POWER_TEST_KEY[] = "sleep_test_arm";

// Variant selector for the deep-sleep test. It is only meaningful while
// NVS_SLEEP_POWER_TEST_KEY is true.
//
//   false -> POWER TEST SLEEP           INA228 stays in continuous conversion
//   true  -> POWER TEST SLEEP INA OFF   INA228 enters its own shutdown mode
//
// One armed flag plus one variant flag, rather than two independent armed
// flags, is what makes "both variants armed at once" unrepresentable.
static constexpr char NVS_SLEEP_INA_OFF_KEY[] = "sleep_ina_off";

static constexpr uint32_t NVS_SCHEMA_VERSION = 2;

// NVS keys. The 15-character limit applies to all of these.
static constexpr char NVS_AUTO_TEST_KEY[] = "auto_test_arm";
static constexpr char NVS_AUTO_INTERVAL_KEY[] = "auto_int_s";
static constexpr char NVS_AUTO_SEQ_HW_KEY[] = "auto_seq_hw";
static constexpr char NVS_BOOT_ID_KEY[] = "boot_id";

// ============================================================================
// SAVE CURRENT STATE TO NVS
// ============================================================================

bool saveCheckpoint(const LoggerCheckpoint &checkpoint)
{
	Serial.println();

	Serial.print("[NVS] Writing checkpoint for experiment ");
	Serial.print(checkpoint.experimentId);
	Serial.print(", interval ");
	Serial.println(checkpoint.completedInterval);

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
	Serial.println(checkpoint.experimentId);

	if (preferences.putUInt(
					"experiment",
					checkpoint.experimentId) != sizeof(uint32_t))
	{

		Serial.println("[ERROR] Failed to write NVS key: experiment");
		ok = false;
	}

	Serial.print("[NVS] Writing interval = ");
	Serial.println(checkpoint.completedInterval);

	if (preferences.putUInt(
					"interval",
					checkpoint.completedInterval) != sizeof(uint32_t))
	{

		Serial.println("[ERROR] Failed to write NVS key: interval");
		ok = false;
	}

	Serial.print("[NVS] Writing charge = ");
	Serial.print(checkpoint.runningCharge_mAh, 6);
	Serial.println(" mAh");

	if (preferences.putDouble(
					"charge",
					checkpoint.runningCharge_mAh) != sizeof(double))
	{

		Serial.println("[ERROR] Failed to write NVS key: charge");
		ok = false;
	}

	Serial.print("[NVS] Writing energy = ");
	Serial.print(checkpoint.runningEnergy_mWh, 6);
	Serial.println(" mWh");

	if (preferences.putDouble(
					"energy",
					checkpoint.runningEnergy_mWh) != sizeof(double))
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

bool loadWifiPowerTestArmed(bool &outArmed)
{
	if (!preferences.begin(NVS_NAMESPACE, true))
	{
		// The message below promises a value, so actually assign it. Leaving the
		// caller's variable untouched here would make the printed claim depend on
		// whatever it already held.
		outArmed = false;
		Serial.println(
				"[NVS] Wi-Fi power-test flag unavailable; defaulting to not armed.");
		return true;
	}

	if (!preferences.isKey(NVS_WIFI_POWER_TEST_KEY))
	{
		preferences.end();
		outArmed = false;
		Serial.println("[NVS] Wi-Fi power-test flag: not armed.");
		return true;
	}

	outArmed = preferences.getBool(NVS_WIFI_POWER_TEST_KEY, false);
	preferences.end();

	Serial.print("[NVS] Wi-Fi power-test flag: ");
	Serial.println(outArmed ? "armed" : "not armed");
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

bool loadSleepPowerTestArmed(bool &outArmed)
{
	if (!preferences.begin(NVS_NAMESPACE, true))
	{
		outArmed = false;
		Serial.println(
				"[NVS] Deep-sleep test flag unavailable; defaulting to not armed.");
		return true;
	}

	if (!preferences.isKey(NVS_SLEEP_POWER_TEST_KEY))
	{
		preferences.end();
		outArmed = false;
		Serial.println("[NVS] Deep-sleep test flag: not armed.");
		return true;
	}

	outArmed = preferences.getBool(NVS_SLEEP_POWER_TEST_KEY, false);
	preferences.end();

	Serial.print("[NVS] Deep-sleep test flag: ");
	Serial.println(outArmed ? "armed" : "not armed");
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

bool loadSleepPowerTestInaOff(bool &outInaOff)
{
	if (!preferences.begin(NVS_NAMESPACE, true))
	{
		outInaOff = false;
		Serial.println(
				"[NVS] Deep-sleep variant flag unavailable; defaulting to INA "
				"continuous.");
		return true;
	}

	if (!preferences.isKey(NVS_SLEEP_INA_OFF_KEY))
	{
		preferences.end();
		outInaOff = false;
		Serial.println("[NVS] Deep-sleep variant flag: INA continuous.");
		return true;
	}

	outInaOff = preferences.getBool(NVS_SLEEP_INA_OFF_KEY, false);
	preferences.end();

	Serial.print("[NVS] Deep-sleep variant flag: ");
	Serial.println(outInaOff ? "INA shutdown" : "INA continuous");
	return true;
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

bool loadCheckpoint(LoggerCheckpoint &out)
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

		out.experimentId = 0;
		out.completedInterval = 0;
		out.runningCharge_mAh = 0.0;
		out.runningEnergy_mWh = 0.0;

		return true;
	}

	Serial.println("[NVS] Namespace opened.");

	if (!preferences.isKey("schema"))
	{
		Serial.println(
				"[WARNING] NVS contains no schema key. Starting from zero.");

		preferences.end();

		out.experimentId = 0;
		out.completedInterval = 0;
		out.runningCharge_mAh = 0.0;
		out.runningEnergy_mWh = 0.0;

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

		out.experimentId = 0;

		out.completedInterval =
				preferences.getUInt("interval", 0);

		out.runningCharge_mAh =
				preferences.getDouble("charge", 0.0);

		out.runningEnergy_mWh =
				preferences.getDouble("energy", 0.0);

		preferences.end();

		Serial.print("[NVS] experiment = ");
		Serial.println(out.experimentId);

		Serial.print("[NVS] interval = ");
		Serial.println(out.completedInterval);

		Serial.print("[NVS] charge = ");
		Serial.print(out.runningCharge_mAh, 6);
		Serial.println(" mAh");

		Serial.print("[NVS] energy = ");
		Serial.print(out.runningEnergy_mWh, 6);
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

		out.experimentId =
				preferences.getUInt("experiment", 0);

		out.completedInterval =
				preferences.getUInt("interval", 0);

		out.runningCharge_mAh =
				preferences.getDouble("charge", 0.0);

		out.runningEnergy_mWh =
				preferences.getDouble("energy", 0.0);

		preferences.end();

		Serial.print("[NVS] experiment = ");
		Serial.println(out.experimentId);

		Serial.print("[NVS] interval = ");
		Serial.println(out.completedInterval);

		Serial.print("[NVS] charge = ");
		Serial.print(out.runningCharge_mAh, 6);
		Serial.println(" mAh");

		Serial.print("[NVS] energy = ");
		Serial.print(out.runningEnergy_mWh, 6);
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

// The read half of what used to be loadAutonomousSettings(). The range check
// that followed it stayed in the sketch with AUTO_INTERVAL_SECONDS_MIN and
// _MAX, which are the cadence authority (D-018) and are used by the LOGGER
// INTERVAL handler as well.
//
// Reporting success after a failed begin() is the behavior that was here
// before the move, defect included. See nvs_persistence.h.
bool loadAutonomousConfig(bool &outArmed,
													uint32_t &outIntervalSeconds,
													uint32_t defaultIntervalSeconds)
{
	if (!preferences.begin(NVS_NAMESPACE, true))
	{
		outArmed = false;
		outIntervalSeconds = defaultIntervalSeconds;
		return true;
	}

	outArmed =
			preferences.isKey(NVS_AUTO_TEST_KEY)
					? preferences.getBool(NVS_AUTO_TEST_KEY, false)
					: false;

	outIntervalSeconds =
			preferences.isKey(NVS_AUTO_INTERVAL_KEY)
					? preferences.getUInt(NVS_AUTO_INTERVAL_KEY,
																defaultIntervalSeconds)
					: defaultIntervalSeconds;

	preferences.end();

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

// The NVS half of reserveSequenceBlock(). How wide a block is, and the
// RTC-retained mirror of the new high-water mark, stayed in the sketch: this
// module stores the number it is given and reports whether the store worked.
bool saveSequenceHighWater(uint32_t highWater)
{
	if (!preferences.begin(NVS_NAMESPACE, false))
	{
		Serial.println("[ERROR] Could not open NVS to reserve a sequence block.");
		return false;
	}

	size_t written = preferences.putUInt(NVS_AUTO_SEQ_HW_KEY, highWater);
	preferences.end();

	if (written != sizeof(uint32_t))
	{
		Serial.println("[ERROR] Failed to reserve a sequence block in NVS.");
		return false;
	}

	return true;
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
