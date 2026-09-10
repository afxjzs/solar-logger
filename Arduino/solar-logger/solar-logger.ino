#include <Wire.h>
#include <Preferences.h>
#include <WiFi.h>
#include "esp_sleep.h"

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
// INA228 REGISTER ADDRESSES
// ============================================================================
//
// The INA228 contains a collection of internal registers.
//
// Think of them as numbered storage locations inside the chip.
//
// Some registers contain configuration:
//   ADC_CONFIG
//   SHUNT_CAL
//
// Some registers contain measurements:
//   VBUS
//   VSHUNT
//   CURRENT
//   POWER
//
// Some registers are accumulators:
//   ENERGY
//   CHARGE
//
// I2C lets us tell the chip:
//   "Give me the bytes stored at register 0x05"
// and then read those bytes back.
// ============================================================================

constexpr uint8_t INA228_ADDRESS = 0x40;

constexpr uint8_t REG_CONFIG          = 0x00;
constexpr uint8_t REG_ADC_CONFIG      = 0x01;
constexpr uint8_t REG_SHUNT_CAL       = 0x02;

constexpr uint8_t REG_VSHUNT          = 0x04;
constexpr uint8_t REG_VBUS            = 0x05;
constexpr uint8_t REG_DIETEMP         = 0x06;
constexpr uint8_t REG_CURRENT         = 0x07;
constexpr uint8_t REG_POWER           = 0x08;
constexpr uint8_t REG_ENERGY          = 0x09;
constexpr uint8_t REG_CHARGE          = 0x0A;

constexpr uint8_t REG_MANUFACTURER_ID = 0x3E;
constexpr uint8_t REG_DEVICE_ID       = 0x3F;


// ============================================================================
// KNOWN-GOOD INA228 CONFIGURATION
// ============================================================================
//
// These values come from the bench configuration we already validated.
//
// ADC_CONFIG = 0xFB6B
//   Continuous bus-voltage, shunt-voltage, and temperature measurement.
//   64-sample averaging.
//
// SHUNT_CAL = 819
//   Calibration value for our effective ~15.62 mOhm shunt.
//
// CURRENT_LSB = 4 uA/count
//   One raw CURRENT-register count represents 4 microamps.
//
// ADCRANGE = 0
//   VSHUNT resolution is 312.5 nV/count.
// ============================================================================

constexpr uint16_t ADC_CONFIG_VALUE = 0xFB6B;
constexpr uint16_t SHUNT_CAL_VALUE  = 819;


// ============================================================================
// INA228 ADC_CONFIG MODE FIELD
// ============================================================================
//
// Source: TI INA228 datasheet SLYS021A (January 2021, revised May 2022),
// Table 7-6, "ADC_CONFIG Register Field Descriptions". Address 1h,
// reset = FB68h.
//
//   Bits 15-12  MODE     (reset Fh)
//   Bits 11-9   VBUSCT   (reset 5h = 1052 us)
//   Bits 8-6    VSHCT    (reset 5h = 1052 us)
//   Bits 5-3    VTCT     (reset 5h = 1052 us)
//   Bits 2-0    AVG      (reset 0h = 1 sample)
//
// The MODE enumeration from that table:
//
//   0h = Shutdown
//   1h = Triggered bus voltage, single shot
//   2h = Triggered shunt voltage, single shot
//   3h = Triggered shunt voltage and bus voltage, single shot
//   4h = Triggered temperature, single shot
//   5h = Triggered temperature and bus voltage, single shot
//   6h = Triggered temperature and shunt voltage, single shot
//   7h = Triggered bus voltage, shunt voltage and temperature, single shot
//   8h = Shutdown
//   9h = Continuous bus voltage only
//   Ah = Continuous shunt voltage only
//   Bh = Continuous shunt and bus voltage
//   Ch = Continuous temperature only
//   Dh = Continuous bus voltage and temperature
//   Eh = Continuous temperature and shunt voltage
//   Fh = Continuous bus voltage, shunt voltage and temperature
//
// NOTE: the datasheet documents TWO shutdown encodings, 0h and 8h. Our
// shutdown verification accepts either, because the test that matters is
// "does the device report a documented shutdown mode", not "does it echo the
// exact bit pattern we happened to choose".
//
// Decoding our own ADC_CONFIG_VALUE of 0xFB6B against that table:
//
//   MODE   = Fh  continuous bus voltage, shunt voltage and temperature
//   VBUSCT = 5h  1052 us
//   VSHCT  = 5h  1052 us
//   VTCT   = 5h  1052 us
//   AVG    = 3h  64 samples
//
// which is exactly what the KNOWN-GOOD block above claims it is.

constexpr uint16_t INA_MODE_MASK  = 0xF000;
constexpr uint8_t  INA_MODE_SHIFT = 12;

constexpr uint16_t INA_MODE_SHUTDOWN_ZERO  = 0x0;
constexpr uint16_t INA_MODE_SHUTDOWN_EIGHT = 0x8;
constexpr uint16_t INA_MODE_CONTINUOUS_ALL = 0xF;


// Datasheet Section 7.4.1 and the Electrical Characteristics table:
//
//   IQ    (active)    640 uA typical, 750 uA maximum, VSENSE = 0 V
//   IQSD  (shutdown)  2.8 uA typical, 5 uA maximum
//   TPOR  from shutdown mode   60 us device start-up time
//
// So the arithmetic prediction for what INA228 shutdown saves is roughly
// 640 uA. The existing 2-second ADC settle in setup() is four orders of
// magnitude longer than the 60 us start-up time, so no new settling delay is
// needed on the wake path.

constexpr double SHUNT_OHMS = 0.01562;

constexpr double CURRENT_LSB_A = 4.0e-6;
constexpr double VSHUNT_LSB_V  = 312.5e-9;
constexpr double VBUS_LSB_V    = 195.3125e-6;
constexpr double DIETEMP_LSB_C = 0.0078125;


// The INA228 POWER and ENERGY register scales are derived from CURRENT_LSB.
//
// POWER_LSB:
//   each POWER-register count represents this many watts.
//
// ENERGY_LSB:
//   each ENERGY-register count represents this many joules.
//
// CHARGE_LSB:
//   each CHARGE-register count represents this many coulombs.
constexpr double POWER_LSB_W  = 3.2 * CURRENT_LSB_A;
constexpr double ENERGY_LSB_J = 16.0 * POWER_LSB_W;
constexpr double CHARGE_LSB_C = CURRENT_LSB_A;


// ADCRANGE = 0 gives us a +/-163.84 mV shunt range.
//
// We use this only for the diagnostic:
//   "Shunt range used: X %"
constexpr double VSHUNT_FULL_SCALE_V = 0.16384;


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
constexpr char NVS_WIFI_POWER_TEST_KEY[]  = "wifi_test_armed";
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

enum PowerTestMode : uint8_t {
  POWER_TEST_NONE = 0,
  POWER_TEST_WIFI = 1,
  POWER_TEST_SLEEP = 2,
  POWER_TEST_CONFLICT = 3,
};

// armedPowerTestMode() and intervalAccountingSuspended() are defined further
// down, alongside the other power-test functions.
//
// They deliberately are NOT defined here. The Arduino sketch preprocessor
// inserts its generated function prototypes immediately before the FIRST
// function definition in the file. A function defined here would push that
// insertion point above "struct SensorReading", and every generated prototype
// that mentions SensorReading would then fail to compile.


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


// ============================================================================
// ONE COMPLETE SENSOR READING
// ============================================================================
//
// A struct is simply a way to group related variables together.
//
// Rather than passing five separate variables around:
//
//   voltage
//   current
//   power
//   temperature
//   shunt voltage
//
// we can pass one SensorReading object.
// ============================================================================

struct SensorReading {
  double voltage_V;
  double current_mA;
  double power_mW;
  double temperature_C;

  int32_t rawVshuntCounts;
  double shuntVoltage_mV;
  double currentFromShunt_mA;
};


// ============================================================================
// SIGN EXTENSION HELPERS
// ============================================================================
//
// Computers need to know whether a binary number is signed.
//
// The INA228 has some signed 20-bit and 40-bit values.
//
// C++ normally works with convenient types such as:
//   int32_t = 32 bits
//   int64_t = 64 bits
//
// Therefore we need to convert the INA228's smaller signed binary number into
// the equivalent normal C++ signed integer.
//
// This is called SIGN EXTENSION.
//
// You do not need to memorize the bit manipulation here. Conceptually:
//
//   INA gives us a signed 20-bit number
//                |
//                v
//   signExtend20()
//                |
//                v
//   normal signed 32-bit C++ integer
// ============================================================================

int32_t signExtend20(uint32_t value) {
  value &= 0xFFFFF;

  // Bit 19 is the sign bit of a 20-bit signed number.
  if (value & 0x80000) {
    value |= 0xFFF00000;
  }

  return static_cast<int32_t>(value);
}


int64_t signExtend40(uint64_t value) {
  value &= 0xFFFFFFFFFFULL;

  // Bit 39 is the sign bit of a 40-bit signed number.
  if (value & 0x8000000000ULL) {
    value |= 0xFFFFFF0000000000ULL;
  }

  return static_cast<int64_t>(value);
}


// ============================================================================
// LOW-LEVEL I2C REGISTER READING
// ============================================================================
//
// This is one of the more firmware-specific parts of the program.
//
// Suppose we want to read VBUS.
//
// VBUS lives in INA228 register 0x05.
//
// The conversation over I2C is approximately:
//
//   ESP32 -> INA228:
//       "I want register 0x05."
//
//   ESP32 -> INA228:
//       "Now send me 3 bytes."
//
//   INA228 -> ESP32:
//       byte 1
//       byte 2
//       byte 3
//
// Those three bytes are then combined into one numeric value.
//
// IMPORTANT:
//
// The "false" passed to:
//
//   Wire.endTransmission(false)
//
// tells the ESP32 NOT to fully release the I2C transaction yet.
//
// This creates what I2C calls a "repeated start":
//
//   START
//   write register address
//   REPEATED START
//   read register data
//   STOP
//
// Many register-based I2C devices expect this pattern.
// ============================================================================

bool readRegisterBytes(uint8_t reg, uint8_t *buffer, size_t length) {
  // Begin addressing the INA228.
  Wire.beginTransmission(INA228_ADDRESS);

  // Send ONE byte containing the register address we want to read.
  Wire.write(reg);

  // false = keep the transaction active for the read that follows.
  uint8_t txResult = Wire.endTransmission(false);

  if (txResult != 0) {
    Serial.print("[ERROR] I2C register-address phase failed for register 0x");
    Serial.print(reg, HEX);
    Serial.print(". Wire error = ");
    Serial.println(txResult);
    return false;
  }


  // Ask the INA228 to return exactly "length" bytes.
  size_t received =
      Wire.requestFrom(
          INA228_ADDRESS,
          static_cast<uint8_t>(length));


  // If we asked for 3 bytes and received only 2, that is a real error.
  //
  // We do NOT silently turn a failed read into a zero measurement.
  if (received != length) {
    Serial.print("[ERROR] I2C short read for register 0x");
    Serial.print(reg, HEX);
    Serial.print(". Expected ");
    Serial.print(length);
    Serial.print(" byte(s), received ");
    Serial.println(received);
    return false;
  }


  // Pull each received byte out of Arduino's internal I2C receive buffer and
  // place it into the array supplied by the caller.
  for (size_t i = 0; i < length; i++) {
    if (!Wire.available()) {
      Serial.print("[ERROR] I2C buffer unexpectedly empty at register 0x");
      Serial.println(reg, HEX);
      return false;
    }

    buffer[i] = Wire.read();
  }

  return true;
}


// ============================================================================
// READING A 16-BIT REGISTER
// ============================================================================
//
// If INA228 sends:
//
//   byte[0] = 0x54
//   byte[1] = 0x49
//
// we need to construct:
//
//   0x5449
//
// "<< 8" means:
//
//   move the first byte left by 8 binary positions
//
// so:
//
//   0x54 becomes 0x5400
//
// then:
//
//   0x5400 | 0x49
//
// becomes:
//
//   0x5449
//
// "|" is bitwise OR. Here it effectively combines the two bytes.
// ============================================================================

bool readRegister16(uint8_t reg, uint16_t &value) {
  uint8_t bytes[2];

  if (!readRegisterBytes(reg, bytes, sizeof(bytes))) {
    return false;
  }

  value =
      (static_cast<uint16_t>(bytes[0]) << 8) |
      bytes[1];

  return true;
}


// ============================================================================
// WRITING A 16-BIT REGISTER
// ============================================================================
//
// This is the opposite operation.
//
// Given:
//
//   value = 0xFB6B
//
// we transmit:
//
//   register address
//   0xFB
//   0x6B
// ============================================================================

bool writeRegister16(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(INA228_ADDRESS);

  Wire.write(reg);

  // Upper byte.
  Wire.write(static_cast<uint8_t>(value >> 8));

  // Lower byte.
  Wire.write(static_cast<uint8_t>(value & 0xFF));

  uint8_t result = Wire.endTransmission();

  if (result != 0) {
    Serial.print("[ERROR] I2C write failed for register 0x");
    Serial.print(reg, HEX);
    Serial.print(". Wire error = ");
    Serial.println(result);
    return false;
  }

  return true;
}


// ============================================================================
// READING INA228 20-BIT MEASUREMENT REGISTERS
// ============================================================================
//
// Several INA228 measurements are 20-bit values.
//
// Physically, I2C still transfers complete bytes, so the INA sends 24 bits:
//
//   byte 1
//   byte 2
//   byte 3
//
// The useful 20-bit value occupies the upper 20 bits.
//
// Therefore:
//
//   raw24 >> 4
//
// discards the four unused low bits.
//
// CURRENT and VSHUNT are SIGNED because current can flow either direction.
//
// VBUS is UNSIGNED because bus voltage is not represented as negative.
// ============================================================================

bool readRegister20Signed(uint8_t reg, int32_t &value) {
  uint8_t bytes[3];

  if (!readRegisterBytes(reg, bytes, sizeof(bytes))) {
    return false;
  }

  uint32_t raw24 =
      (static_cast<uint32_t>(bytes[0]) << 16) |
      (static_cast<uint32_t>(bytes[1]) << 8) |
      bytes[2];

  uint32_t raw20 = raw24 >> 4;

  value = signExtend20(raw20);

  return true;
}


bool readRegister20Unsigned(uint8_t reg, uint32_t &value) {
  uint8_t bytes[3];

  if (!readRegisterBytes(reg, bytes, sizeof(bytes))) {
    return false;
  }

  uint32_t raw24 =
      (static_cast<uint32_t>(bytes[0]) << 16) |
      (static_cast<uint32_t>(bytes[1]) << 8) |
      bytes[2];

  value = raw24 >> 4;

  return true;
}


// POWER uses the entire unsigned 24-bit value.
bool readRegister24Unsigned(uint8_t reg, uint32_t &value) {
  uint8_t bytes[3];

  if (!readRegisterBytes(reg, bytes, sizeof(bytes))) {
    return false;
  }

  value =
      (static_cast<uint32_t>(bytes[0]) << 16) |
      (static_cast<uint32_t>(bytes[1]) << 8) |
      bytes[2];

  return true;
}


// ENERGY and CHARGE are 40-bit registers.
//
// Five bytes × 8 bits = 40 bits.
//
// Each pass through the loop shifts the number left by one byte and appends
// the next byte.
bool readRegister40Unsigned(uint8_t reg, uint64_t &value) {
  uint8_t bytes[5];

  if (!readRegisterBytes(reg, bytes, sizeof(bytes))) {
    return false;
  }

  value = 0;

  for (uint8_t byte : bytes) {
    value = (value << 8) | byte;
  }

  return true;
}


bool readRegister40Signed(uint8_t reg, int64_t &value) {
  uint64_t raw;

  if (!readRegister40Unsigned(reg, raw)) {
    return false;
  }

  value = signExtend40(raw);

  return true;
}


// ============================================================================
// INA228 IDENTITY CHECK
// ============================================================================

bool verifyInaIdentity() {
  Serial.println("[INA228] Verifying device identity...");

  uint16_t manufacturerId;
  uint16_t deviceId;

  if (!readRegister16(REG_MANUFACTURER_ID, manufacturerId) ||
      !readRegister16(REG_DEVICE_ID, deviceId)) {

    Serial.println("[ERROR] Could not read INA228 identity registers.");
    return false;
  }


  Serial.print("[INA228] Manufacturer ID = 0x");
  Serial.println(manufacturerId, HEX);

  Serial.print("[INA228] Device ID       = 0x");
  Serial.println(deviceId, HEX);


  // TI's manufacturer ID is ASCII "TI":
  //
  //   'T' = 0x54
  //   'I' = 0x49
  //
  // giving:
  //
  //   0x5449
  if (manufacturerId != 0x5449) {
    Serial.println(
        "[ERROR] Manufacturer ID is not Texas Instruments (0x5449).");
    return false;
  }


  // DEVICE_ID contains:
  //
  // upper 12 bits = device number
  // lower 4 bits  = revision
  //
  // Example:
  //
  //   0x2281
  //
  // means:
  //
  //   device   = 0x228
  //   revision = 1
  uint16_t deviceNumber = deviceId >> 4;
  uint8_t revision = deviceId & 0x0F;


  if (deviceNumber != 0x228) {
    Serial.print("[ERROR] Device number is 0x");
    Serial.print(deviceNumber, HEX);
    Serial.println(", expected INA228 device number 0x228.");
    return false;
  }


  Serial.print("[INA228] Revision        = ");
  Serial.println(revision);

  Serial.println("[INA228] Identity check: PASS");

  return true;
}


// ============================================================================
// INA228 CONFIGURATION
// ============================================================================

bool configureIna228() {
  Serial.println("[INA228] Configuring sensor...");


  // CONFIG = 0 keeps ADCRANGE = 0.
  //
  // Our calibrated shunt-voltage scale depends on this.
  if (!writeRegister16(REG_CONFIG, 0x0000)) {
    return false;
  }


  if (!writeRegister16(REG_ADC_CONFIG, ADC_CONFIG_VALUE)) {
    return false;
  }


  if (!writeRegister16(REG_SHUNT_CAL, SHUNT_CAL_VALUE)) {
    return false;
  }


  // Now READ BACK the values we just wrote.
  //
  // This matters because:
  //
  //   "I sent the bytes successfully"
  //
  // is not quite the same proof as:
  //
  //   "The device now contains the configuration I expected."
  uint16_t configReadback;
  uint16_t adcReadback;
  uint16_t shuntCalReadback;


  if (!readRegister16(REG_CONFIG, configReadback) ||
      !readRegister16(REG_ADC_CONFIG, adcReadback) ||
      !readRegister16(REG_SHUNT_CAL, shuntCalReadback)) {

    Serial.println("[ERROR] Could not read INA228 configuration back.");
    return false;
  }


  // CONFIG bit 4 is ADCRANGE.
  if ((configReadback & 0x0010) != 0) {
    Serial.println(
        "[ERROR] INA228 ADCRANGE is not 0. Calibration would be wrong.");
    return false;
  }


  if (adcReadback != ADC_CONFIG_VALUE) {
    Serial.print("[ERROR] ADC_CONFIG readback mismatch. Got 0x");
    Serial.print(adcReadback, HEX);
    Serial.print(", expected 0x");
    Serial.println(ADC_CONFIG_VALUE, HEX);
    return false;
  }


  if (shuntCalReadback != SHUNT_CAL_VALUE) {
    Serial.print("[ERROR] SHUNT_CAL readback mismatch. Got ");
    Serial.print(shuntCalReadback);
    Serial.print(", expected ");
    Serial.println(SHUNT_CAL_VALUE);
    return false;
  }


  Serial.println(
      "[INA228] CONFIG / ADC_CONFIG / SHUNT_CAL readback: PASS");

  Serial.println("[INA228] Configuration complete.");

  return true;
}


// ============================================================================
// INA228 SHUTDOWN MODE
// ============================================================================
//
// Used by the INA-OFF variant of the deep-sleep power test to measure the
// lowest power reachable WITHOUT physically power-gating the INA228.
//
// This never removes INA228 power, never touches the XIAO 3V3 rail, and never
// disconnects SDA or SCL. It writes the documented shutdown MODE and nothing
// else: every other ADC_CONFIG bit is preserved.
//
// MEASUREMENT SEMANTICS, and they are not subtle:
//
//   While the INA228 is shut down, no ADC conversions occur. Voltage,
//   current, power, and temperature registers stop being updated, and the
//   hardware CHARGE and ENERGY accumulators stop accumulating. The registers
//   still read back, but their contents are STALE, not new measurements.
//
// This variant deliberately sacrifices solar measurement during the sleep
// phase. That is the entire point of the experiment.
// ============================================================================

uint16_t inaModeFromAdcConfig(uint16_t adcConfig) {
  return (adcConfig & INA_MODE_MASK) >> INA_MODE_SHIFT;
}


bool inaModeIsShutdown(uint16_t mode) {
  return mode == INA_MODE_SHUTDOWN_ZERO || mode == INA_MODE_SHUTDOWN_EIGHT;
}


void printAdcConfigHex(const char *label, uint16_t value) {
  Serial.print(label);
  Serial.print("0x");

  // Print a fixed four-digit hex value. Serial.print(x, HEX) drops leading
  // zeros, which would make 0x0B6B display as "B6B" and read like a
  // different register width.
  for (int8_t nibble = 3; nibble >= 0; nibble--) {
    uint8_t digit = (value >> (nibble * 4)) & 0x0F;
    Serial.print(digit, HEX);
  }

  Serial.println();
}


bool enterInaShutdownMode() {
  Serial.println("[INA228] Entering shutdown mode...");

  uint16_t before;

  if (!readRegister16(REG_ADC_CONFIG, before)) {
    Serial.println(
        "[INA228] ERROR: Could not read ADC_CONFIG before shutdown.");
    return false;
  }

  printAdcConfigHex("[INA228] ADC_CONFIG before: ", before);

  // Replace ONLY the MODE field. VBUSCT, VSHCT, VTCT, and AVG are carried
  // through untouched so that waking into the normal configuration does not
  // depend on this function having preserved them by accident.
  uint16_t desired =
      (before & ~INA_MODE_MASK) |
      (static_cast<uint16_t>(INA_MODE_SHUTDOWN_ZERO) << INA_MODE_SHIFT);

  if (!writeRegister16(REG_ADC_CONFIG, desired)) {
    Serial.println(
        "[INA228] ERROR: Could not write ADC_CONFIG shutdown mode.");
    return false;
  }

  uint16_t after;

  if (!readRegister16(REG_ADC_CONFIG, after)) {
    Serial.println(
        "[INA228] ERROR: Could not read ADC_CONFIG back after shutdown "
        "write.");
    return false;
  }

  printAdcConfigHex("[INA228] ADC_CONFIG after:  ", after);

  uint16_t mode = inaModeFromAdcConfig(after);

  if (!inaModeIsShutdown(mode)) {
    Serial.print(
        "[INA228] ERROR: MODE bits do not indicate shutdown. MODE = 0x");
    Serial.println(mode, HEX);
    Serial.println(
        "[INA228] Datasheet SLYS021A Table 7-6 documents 0h and 8h as "
        "Shutdown.");
    return false;
  }

  // A shutdown write that also changed conversion times or averaging would
  // mean the next wake restores something other than the known-good
  // configuration. Catch that here rather than discovering it in the data.
  if ((after & ~INA_MODE_MASK) != (before & ~INA_MODE_MASK)) {
    Serial.println(
        "[INA228] ERROR: Non-MODE ADC_CONFIG bits changed during the "
        "shutdown write.");
    printAdcConfigHex("[INA228] Expected non-MODE bits: ",
                      static_cast<uint16_t>(before & ~INA_MODE_MASK));
    printAdcConfigHex("[INA228] Actual non-MODE bits:   ",
                      static_cast<uint16_t>(after & ~INA_MODE_MASK));
    return false;
  }

  Serial.println("[INA228] Shutdown mode verified: ALL OK");
  Serial.println(
      "[INA228] Measurement SUSPENDED: no conversions, no CHARGE/ENERGY "
      "accumulation.");
  Serial.println(
      "[INA228] Register contents are now stale and must not be reported as "
      "new measurements.");

  return true;
}


// Read back what the device is actually doing. Nothing here asserts a state;
// it reports the MODE field that was read from the part.
bool inaIsInContinuousMode(bool verbose) {
  uint16_t adcConfig;

  if (!readRegister16(REG_ADC_CONFIG, adcConfig)) {
    Serial.println(
        "[INA228] ERROR: Could not read ADC_CONFIG to confirm mode.");
    return false;
  }

  uint16_t mode = inaModeFromAdcConfig(adcConfig);

  if (verbose) {
    printAdcConfigHex("[INA228] ADC_CONFIG now:    ", adcConfig);
  }

  return mode == INA_MODE_CONTINUOUS_ALL;
}


// Bring the INA228 back to the project's standard continuous configuration.
//
// The normal wake path already does this, because setup() calls
// configureIna228() on every boot. This exists for the paths that do NOT go
// through a reboot: stopping the test while awake, and the failure path where
// shutdown succeeded but the ESP32 then did not sleep.
bool restoreInaContinuousMode() {
  if (inaIsInContinuousMode(false)) {
    Serial.println(
        "[INA228] Continuous measurement already active: ALL OK");
    return true;
  }

  Serial.println(
      "[INA228] INA228 is not in continuous mode. Restoring configuration...");

  if (!configureIna228()) {
    Serial.println(
        "[INA228] ERROR: Could not restore INA228 configuration.");
    return false;
  }

  if (!inaIsInContinuousMode(true)) {
    Serial.println(
        "[INA228] ERROR: INA228 still does not report continuous mode after "
        "reconfiguration.");
    return false;
  }

  Serial.println("[INA228] Continuous measurement restored: ALL OK");
  return true;
}


// Print the raw ADC_CONFIG and the decoded MODE field, for the case where the
// device is in some state other than continuous conversion. Saying only "not
// continuous" would leave the operator with nowhere to go.
void printPowerTestInaModeDetail() {
  uint16_t adcConfig;

  if (!readRegister16(REG_ADC_CONFIG, adcConfig)) {
    Serial.println(
        "[INA228] ERROR: Could not read ADC_CONFIG for mode detail.");
    return;
  }

  printAdcConfigHex("[INA228] ADC_CONFIG now:    ", adcConfig);

  uint16_t mode = inaModeFromAdcConfig(adcConfig);

  Serial.print("[INA228] MODE field: 0x");
  Serial.print(mode, HEX);

  if (inaModeIsShutdown(mode)) {
    Serial.println(" (Shutdown)");
    Serial.println(
        "[INA228] Measurement is SUSPENDED. Register values are stale.");
  } else if (mode == INA_MODE_CONTINUOUS_ALL) {
    Serial.println(" (Continuous bus voltage, shunt voltage and temperature)");
  } else {
    Serial.println(" (see datasheet SLYS021A Table 7-6)");
  }
}


// ============================================================================
// RESET INA228 ENERGY AND CHARGE ACCUMULATORS
// ============================================================================
//
// CONFIG bit 14 is RSTACC.
//
// Setting that bit tells the INA228:
//
//   "Clear the hardware ENERGY and CHARGE counters."
//
// It does NOT reboot the sensor.
//
// It does NOT erase SHUNT_CAL.
//
// It does NOT erase ADC_CONFIG.
//
// The bit automatically clears itself after performing the reset.
// ============================================================================

bool resetInaAccumulators() {
  Serial.println(
      "[INA228] Resetting ENERGY and CHARGE accumulators...");


  uint16_t config;

  if (!readRegister16(REG_CONFIG, config)) {
    Serial.println(
        "[ERROR] Cannot reset accumulators because CONFIG read failed.");
    return false;
  }


  if (!writeRegister16(REG_CONFIG, config | 0x4000)) {
    Serial.println("[ERROR] Accumulator reset command failed.");
    return false;
  }


  // Give the INA228 a moment to execute the reset.
  delay(2);


  // Prove that the reset actually happened by reading the hardware accumulators
  // back from the INA228.
  uint64_t energyRaw;
  int64_t chargeRaw;


  if (!readRegister40Unsigned(REG_ENERGY, energyRaw) ||
      !readRegister40Signed(REG_CHARGE, chargeRaw)) {

    Serial.println("[ERROR] Could not verify accumulator reset.");
    return false;
  }


  // Because the ADC is running continuously, it is theoretically possible for
  // accumulation to begin again very quickly.
  //
  // Normally we see exactly zero immediately after reset.
  if (energyRaw != 0 || chargeRaw != 0) {
    Serial.print(
        "[WARNING] Accumulator reset readback was not exactly zero. ENERGY=");
    Serial.print(static_cast<unsigned long long>(energyRaw));

    Serial.print(" CHARGE=");
    Serial.println(static_cast<long long>(chargeRaw));
  } else {
    Serial.println("[INA228] Accumulators reset: OK");
  }


  return true;
}


// ============================================================================
// READ ALL LIVE SENSOR VALUES
// ============================================================================

bool readSensor(SensorReading &reading, bool verboseVshunt) {
  uint32_t rawBus;
  int32_t rawCurrent;
  uint32_t rawPower;
  uint16_t rawTempUnsigned;
  int32_t rawVshunt;


  if (!readRegister20Unsigned(REG_VBUS, rawBus) ||
      !readRegister20Signed(REG_CURRENT, rawCurrent) ||
      !readRegister24Unsigned(REG_POWER, rawPower) ||
      !readRegister16(REG_DIETEMP, rawTempUnsigned) ||
      !readRegister20Signed(REG_VSHUNT, rawVshunt)) {

    Serial.println("[ERROR] Sensor measurement read failed.");
    return false;
  }


  // The raw temperature bits represent a signed 16-bit value.
  int16_t rawTemp =
      static_cast<int16_t>(rawTempUnsigned);


  // Raw ADC counts become physical measurements by multiplying by the
  // appropriate datasheet scale.
  reading.voltage_V =
      rawBus * VBUS_LSB_V;


  reading.current_mA =
      rawCurrent *
      CURRENT_LSB_A *
      1000.0;


  reading.power_mW =
      rawPower *
      POWER_LSB_W *
      1000.0;


  reading.temperature_C =
      rawTemp *
      DIETEMP_LSB_C;


  reading.rawVshuntCounts =
      rawVshunt;


  reading.shuntVoltage_mV =
      rawVshunt *
      VSHUNT_LSB_V *
      1000.0;


  // Independent-looking diagnostic calculation:
  //
  //   I = V / R
  //
  // This converts measured shunt voltage into current.
  //
  // Remember that it uses our assumed SHUNT_OHMS value, so this is excellent
  // for detecting internal inconsistencies but is not a completely independent
  // calibration reference.
  reading.currentFromShunt_mA =
      (reading.shuntVoltage_mV / 1000.0) /
      SHUNT_OHMS *
      1000.0;


  if (verboseVshunt) {
    Serial.println("[VSHUNT] Reading raw shunt ADC...");

    Serial.print("[VSHUNT] Signed ADC counts    = ");
    Serial.println(reading.rawVshuntCounts);

    Serial.print("[VSHUNT] Shunt voltage        = ");
    Serial.print(reading.shuntVoltage_mV, 6);
    Serial.println(" mV");

    Serial.print("[VSHUNT] Assumed shunt R      = ");
    Serial.print(SHUNT_OHMS * 1000.0, 4);
    Serial.println(" mOhm");

    Serial.print("[VSHUNT] V/R current          = ");
    Serial.print(reading.currentFromShunt_mA, 3);
    Serial.println(" mA");

    Serial.print("[CURRENT] INA CURRENT register = ");
    Serial.print(reading.current_mA, 3);
    Serial.println(" mA");

    Serial.print("[COMPARE] Difference           = ");
    Serial.print(
        reading.current_mA -
        reading.currentFromShunt_mA,
        3);
    Serial.println(" mA");
  }


  return true;
}

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
void printLiveSample() {
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
  if (sleepPowerTestRunning) {
    return;
  }

  SensorReading reading;

  // readSensor() fills the SensorReading struct with the latest values
  // currently available from the INA228.
  //
  // false means:
  //   Do not print the verbose VSHUNT diagnostic block for every 1-second
  //   sample. That would create a huge amount of Serial output.
  if (!readSensor(reading, false)) {
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
// READ INA228 HARDWARE CHARGE ACCUMULATOR
// ============================================================================

bool readAccumulatedCharge_mAh(double &charge_mAh) {
  int64_t rawCharge;

  if (!readRegister40Signed(REG_CHARGE, rawCharge)) {
    return false;
  }


  // INA228 CHARGE is stored in coulombs.
  //
  // 1 amp-hour = 3600 coulombs
  // 1 mAh      = 3.6 coulombs
  //
  // Therefore:
  //
  //   mAh = coulombs / 3.6
  charge_mAh =
      rawCharge *
      CHARGE_LSB_C /
      3.6;


  return true;
}


// ============================================================================
// READ INA228 HARDWARE ENERGY ACCUMULATOR
// ============================================================================

bool readAccumulatedEnergy_mWh(double &energy_mWh) {
  uint64_t rawEnergy;

  if (!readRegister40Unsigned(REG_ENERGY, rawEnergy)) {
    return false;
  }


  // INA228 ENERGY is stored in joules.
  //
  // 1 Wh  = 3600 joules
  // 1 mWh = 3.6 joules
  energy_mWh =
      rawEnergy *
      ENERGY_LSB_J /
      3.6;


  return true;
}


// ============================================================================
// SAVE CURRENT STATE TO NVS
// ============================================================================

bool saveCheckpoint() {
  Serial.println();

  Serial.print("[NVS] Writing checkpoint for experiment ");
  Serial.print(experimentId);
  Serial.print(", interval ");
  Serial.println(completedInterval);


  // false means open the namespace READ/WRITE.
  if (!preferences.begin(NVS_NAMESPACE, false)) {
    Serial.println(
        "[ERROR] Could not open NVS namespace for writing.");
    return false;
  }


  bool ok = true;


  Serial.print("[NVS] Writing schema = ");
  Serial.println(NVS_SCHEMA_VERSION);

  if (preferences.putUInt(
          "schema",
          NVS_SCHEMA_VERSION) != sizeof(uint32_t)) {

    Serial.println("[ERROR] Failed to write NVS key: schema");
    ok = false;
  }


  Serial.print("[NVS] Writing experiment = ");
  Serial.println(experimentId);

  if (preferences.putUInt(
          "experiment",
          experimentId) != sizeof(uint32_t)) {

    Serial.println("[ERROR] Failed to write NVS key: experiment");
    ok = false;
  }


  Serial.print("[NVS] Writing interval = ");
  Serial.println(completedInterval);

  if (preferences.putUInt(
          "interval",
          completedInterval) != sizeof(uint32_t)) {

    Serial.println("[ERROR] Failed to write NVS key: interval");
    ok = false;
  }


  Serial.print("[NVS] Writing charge = ");
  Serial.print(runningCharge_mAh, 6);
  Serial.println(" mAh");

  if (preferences.putDouble(
          "charge",
          runningCharge_mAh) != sizeof(double)) {

    Serial.println("[ERROR] Failed to write NVS key: charge");
    ok = false;
  }


  Serial.print("[NVS] Writing energy = ");
  Serial.print(runningEnergy_mWh, 6);
  Serial.println(" mWh");

  if (preferences.putDouble(
          "energy",
          runningEnergy_mWh) != sizeof(double)) {

    Serial.println("[ERROR] Failed to write NVS key: energy");
    ok = false;
  }


  // Flush/close the Preferences namespace.
  preferences.end();


  if (ok) {
    Serial.println(
        "*** NVS CHECKPOINT SAVED SUCCESSFULLY ***");
  } else {
    Serial.println(
        "*** ERROR: NVS CHECKPOINT WAS NOT FULLY SAVED ***");
  }


  return ok;
}


// The Wi-Fi power-test flag is stored under its own key. It is deliberately
// separate from the experiment checkpoint so arming or stopping this bench
// test cannot alter experiment totals, interval numbers, or experiment IDs.
bool saveWifiPowerTestArmed(bool armed) {
  Serial.print("[NVS] Writing ");
  Serial.print(NVS_WIFI_POWER_TEST_KEY);
  Serial.print(" = ");
  Serial.println(armed ? "true" : "false");

  if (!preferences.begin(NVS_NAMESPACE, false)) {
    Serial.println("[ERROR] Could not open NVS for Wi-Fi power-test flag.");
    return false;
  }

  size_t written = preferences.putBool(NVS_WIFI_POWER_TEST_KEY, armed);
  preferences.end();

  if (written != sizeof(bool)) {
    Serial.println("[ERROR] Failed to write Wi-Fi power-test NVS flag.");
    return false;
  }

  Serial.println("[NVS] Wi-Fi power-test flag saved: ALL OK");
  return true;
}


bool loadWifiPowerTestArmed() {
  if (!preferences.begin(NVS_NAMESPACE, true)) {
    // The message below promises a value, so actually assign it. Leaving the
    // global untouched here would make the printed claim depend on whatever
    // the variable already held.
    wifiPowerTestArmed = false;
    Serial.println(
        "[NVS] Wi-Fi power-test flag unavailable; defaulting to not armed.");
    return true;
  }

  if (!preferences.isKey(NVS_WIFI_POWER_TEST_KEY)) {
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
bool saveSleepPowerTestArmed(bool armed) {
  Serial.print("[NVS] Writing ");
  Serial.print(NVS_SLEEP_POWER_TEST_KEY);
  Serial.print(" = ");
  Serial.println(armed ? "true" : "false");

  if (!preferences.begin(NVS_NAMESPACE, false)) {
    Serial.println("[ERROR] Could not open NVS for deep-sleep test flag.");
    return false;
  }

  size_t written = preferences.putBool(NVS_SLEEP_POWER_TEST_KEY, armed);
  preferences.end();

  if (written != sizeof(bool)) {
    Serial.println("[ERROR] Failed to write deep-sleep test NVS flag.");
    return false;
  }

  Serial.println("[NVS] Deep-sleep test flag saved: ALL OK");
  return true;
}


bool loadSleepPowerTestArmed() {
  if (!preferences.begin(NVS_NAMESPACE, true)) {
    sleepPowerTestArmed = false;
    Serial.println(
        "[NVS] Deep-sleep test flag unavailable; defaulting to not armed.");
    return true;
  }

  if (!preferences.isKey(NVS_SLEEP_POWER_TEST_KEY)) {
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


bool saveSleepPowerTestInaOff(bool inaOff) {
  Serial.print("[NVS] Writing ");
  Serial.print(NVS_SLEEP_INA_OFF_KEY);
  Serial.print(" = ");
  Serial.println(inaOff ? "true" : "false");

  if (!preferences.begin(NVS_NAMESPACE, false)) {
    Serial.println("[ERROR] Could not open NVS for deep-sleep variant flag.");
    return false;
  }

  size_t written = preferences.putBool(NVS_SLEEP_INA_OFF_KEY, inaOff);
  preferences.end();

  if (written != sizeof(bool)) {
    Serial.println("[ERROR] Failed to write deep-sleep variant NVS flag.");
    return false;
  }

  Serial.println("[NVS] Deep-sleep variant flag saved: ALL OK");
  return true;
}


bool loadSleepPowerTestInaOff() {
  if (!preferences.begin(NVS_NAMESPACE, true)) {
    sleepPowerTestInaOff = false;
    Serial.println(
        "[NVS] Deep-sleep variant flag unavailable; defaulting to INA "
        "continuous.");
    return true;
  }

  if (!preferences.isKey(NVS_SLEEP_INA_OFF_KEY)) {
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
const char *sleepPowerTestVariantName() {
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

bool loadCheckpoint() {
  Serial.println();
  Serial.println("[NVS] Beginning checkpoint load...");

  Serial.print("[NVS] Opening namespace \"");
  Serial.print(NVS_NAMESPACE);
  Serial.println("\" in READ ONLY mode...");


  // true = READ ONLY.
  if (!preferences.begin(NVS_NAMESPACE, true)) {
    Serial.println(
        "[WARNING] NVS namespace does not exist yet. Starting from zero.");

    experimentId = 0;
    completedInterval = 0;
    runningCharge_mAh = 0.0;
    runningEnergy_mWh = 0.0;

    return true;
  }


  Serial.println("[NVS] Namespace opened.");


  if (!preferences.isKey("schema")) {
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

  if (schema == 1) {
    Serial.println(
        "[NVS] Legacy schema 1 detected.");

    Serial.println(
        "[NVS] Loading old interval/charge/energy values.");

    Serial.println(
        "[NVS] Legacy data will be treated as experiment 0.");


    if (!preferences.isKey("interval") ||
        !preferences.isKey("charge") ||
        !preferences.isKey("energy")) {

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

  if (schema == NVS_SCHEMA_VERSION) {
    if (!preferences.isKey("experiment") ||
        !preferences.isKey("interval") ||
        !preferences.isKey("charge") ||
        !preferences.isKey("energy")) {

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

void printCsvHeader() {
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
void printExperimentStartEvent() {
  Serial.print("CSV_EVENT,EXPERIMENT_START,");
  Serial.println(experimentId);
}


// Explicit machine-readable event showing that an existing experiment was
// resumed after an ESP32 reboot.
void printExperimentResumeEvent() {
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
void printSleepTestEvent(const char *eventType, uint32_t cycle) {
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
    double averagePower_mW) {

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

const char *wifiModeName(wifi_mode_t mode) {
  switch (mode) {
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


void printWifiStatus() {
  wifi_mode_t mode = WiFi.getMode();

  Serial.print("[WIFI] State: ");
  Serial.println(mode == WIFI_OFF ? "DISABLED" : "ENABLED");

  Serial.print("[WIFI] Mode: ");
  Serial.println(wifiModeName(mode));
}


void enableWifi() {
  Serial.println("[WIFI] Enabling Wi-Fi...");

  // STA means station mode: the ESP32 behaves like a client radio, but this
  // command deliberately does not join an access point. Enabling the radio
  // without a network connection lets bench measurements isolate Wi-Fi power.
  if (!WiFi.mode(WIFI_STA)) {
    Serial.println("[WIFI] ERROR: Could not set station mode.");
    return;
  }

  if (!WiFi.disconnect(false, false)) {
    Serial.println("[WIFI] ERROR: Could not keep Wi-Fi disconnected.");
    return;
  }

  Serial.println("[WIFI] Mode: STA");
  Serial.println("[WIFI] Wi-Fi enabled: ALL OK");
}


void disableWifi() {
  Serial.println("[WIFI] Disabling Wi-Fi...");

  // WIFI_OFF is the Arduino ESP32 core's explicit radio-off mode.
  if (!WiFi.mode(WIFI_OFF)) {
    Serial.println("[WIFI] ERROR: Could not disable Wi-Fi.");
    return;
  }

  Serial.println("[WIFI] Wi-Fi disabled: ALL OK");
}


void initializeWifiOff() {
  // Set the baseline explicitly instead of relying on framework defaults,
  // which can vary after resets or across Arduino core versions.
  Serial.println("[WIFI] Boot default: OFF");
  disableWifi();
}


void startWifiPowerTestPhase(uint8_t phase) {
  wifiPowerTestPhase = phase;
  wifiPowerTestPhaseStartedMs = millis();

  if (phase == 1) {
    // Explicitly turn the radio OFF for each OFF phase, including adjacent
    // OFF phases at the boundary between repeated cycles.
    disableWifi();
    Serial.println("[POWER TEST] Phase 1/3: WIFI OFF for 30 seconds");

  } else if (phase == 2) {
    enableWifi();
    Serial.println("[POWER TEST] Phase 2/3: WIFI ON for 30 seconds");

  } else if (phase == 3) {
    disableWifi();
    Serial.println("[POWER TEST] Phase 3/3: WIFI OFF for 30 seconds");

  } else {
    Serial.print("[POWER TEST] ERROR: Invalid phase: ");
    Serial.println(phase);
  }
}


void updateWifiPowerTest(unsigned long now) {
  if (!wifiPowerTestRunning ||
      now - wifiPowerTestPhaseStartedMs < WIFI_POWER_TEST_PHASE_MS) {
    return;
  }

  if (wifiPowerTestPhase == 1) {
    startWifiPowerTestPhase(2);

  } else if (wifiPowerTestPhase == 2) {
    startWifiPowerTestPhase(3);

  } else if (wifiPowerTestPhase == 3) {
    Serial.println("[POWER TEST] Cycle complete. Repeating...");
    startWifiPowerTestPhase(1);

  } else {
    Serial.println("[POWER TEST] ERROR: Stopping due to invalid phase.");
    wifiPowerTestRunning = false;
  }
}


void armWifiPowerTest() {
  // Only one power test may be armed at a time. Silently replacing an armed
  // test would change what the DMM is measuring without saying so.
  if (sleepPowerTestArmed || sleepPowerTestRunning) {
    Serial.println(
        "[POWER TEST] ERROR: The deep-sleep power test is already armed.");
    Serial.println(
        "[POWER TEST] Send POWER TEST STOP first, then POWER TEST WIFI.");
    return;
  }

  if (!saveWifiPowerTestArmed(true)) {
    Serial.println("[POWER TEST] ERROR: Test was not armed.");
    return;
  }

  wifiPowerTestArmed = true;
  wifiPowerTestRunning = true;
  Serial.println("[POWER TEST] Wi-Fi power test ARMED");
  startWifiPowerTestPhase(1);
}


// NOTE: The former stopWifiPowerTest() was removed when POWER TEST STOP began
// handling both power tests. Its logic now lives in stopAllPowerTests(), which
// is the single stop path. Two stop functions would eventually diverge.


void printWifiPowerTestStatus() {
  Serial.print("[POWER TEST] Wi-Fi test armed: ");
  Serial.println(wifiPowerTestArmed ? "YES" : "NO");

  Serial.print("[POWER TEST] Wi-Fi test running: ");
  Serial.println(wifiPowerTestRunning ? "YES" : "NO");

  if (!wifiPowerTestRunning) {
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

PowerTestMode armedPowerTestMode() {
  if (wifiPowerTestArmed && sleepPowerTestArmed) {
    return POWER_TEST_CONFLICT;
  }

  if (wifiPowerTestArmed) {
    return POWER_TEST_WIFI;
  }

  if (sleepPowerTestArmed) {
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

bool intervalAccountingSuspended() {
  return sleepPowerTestRunning || intervalAccountingBlocked;
}


const char *intervalAccountingSuspendReason() {
  if (sleepPowerTestRunning) {
    return "deep-sleep power test in progress";
  }

  if (intervalAccountingBlocked) {
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

const char *wakeupCauseName(esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
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


void printBootWakeReason() {
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

void startSleepPowerTestAwakePhase() {
  sleepPowerTestRunning = true;
  sleepPowerTestAwakeStartedMs = millis();

  Serial.println();

  if (sleepPowerTestInaOff) {
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

  if (mode != WIFI_OFF) {
    Serial.println(
        "[POWER TEST] ERROR: Wi-Fi must be OFF during the sleep test.");
    Serial.println(
        "[POWER TEST] Disabling Wi-Fi now so the measurement stays valid.");

    disableWifi();

    if (WiFi.getMode() != WIFI_OFF) {
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

  if (sleepPowerTestInaOff) {
    Serial.println(
        "[POWER TEST] During each deep sleep the INA228 will be in its own "
        "shutdown mode.");
    Serial.println(
        "[POWER TEST] Solar measurement is therefore SUSPENDED during sleep: "
        "no conversions and no CHARGE/ENERGY accumulation.");
    Serial.println(
        "[POWER TEST] The INA228 measures normally during this awake phase.");
  } else {
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

void enterSleepPowerTestDeepSleep() {
  Serial.println();

  if (sleepPowerTestInaOff) {
    Serial.println("[POWER TEST] INA-OFF sleep test");

    // The INA228 is shut down BEFORE the wake timer is armed, so a shutdown
    // failure costs nothing: the board simply never sleeps this cycle.
    if (!enterInaShutdownMode()) {
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

  if (timerResult != ESP_OK) {
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


void updateSleepPowerTest(unsigned long now) {
  if (!sleepPowerTestRunning) {
    return;
  }

  if (now - sleepPowerTestAwakeStartedMs < SLEEP_POWER_TEST_AWAKE_MS) {
    return;
  }

  enterSleepPowerTestDeepSleep();
}


void armSleepPowerTest(bool inaOff) {
  if (wifiPowerTestArmed || wifiPowerTestRunning) {
    Serial.println(
        "[POWER TEST] ERROR: The Wi-Fi power test is already armed.");
    Serial.println(
        "[POWER TEST] Send POWER TEST STOP first, then arm the sleep test.");
    return;
  }

  if (sleepPowerTestArmed) {
    if (sleepPowerTestInaOff == inaOff) {
      Serial.print(
          "[POWER TEST] Deep-sleep power test is already armed in this "
          "variant: ");
      Serial.println(sleepPowerTestVariantName());
      Serial.println("[POWER TEST] No change made.");
      return;
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
    return;
  }

  // Write the variant BEFORE the armed flag. If the variant write fails the
  // test is not armed at all, so a reboot can never resume a test whose
  // variant is unknown.
  if (!saveSleepPowerTestInaOff(inaOff)) {
    Serial.println(
        "[POWER TEST] ERROR: Deep-sleep variant was NOT saved, so the test "
        "was not armed.");
    return;
  }

  sleepPowerTestInaOff = inaOff;

  if (!saveSleepPowerTestArmed(true)) {
    Serial.println(
        "[POWER TEST] ERROR: Deep-sleep test was NOT armed. It would not "
        "survive the USB disconnect, so it is not being started either.");
    return;
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

  if (inaOff) {
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

bool resumeIntervalAccounting() {
  Serial.println(
      "[POWER TEST] Resuming normal 60-second interval accounting.");

  if (!resetInaAccumulators()) {
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

void stopAllPowerTests() {
  bool wifiTestWasActive = wifiPowerTestArmed || wifiPowerTestRunning;
  bool sleepTestWasActive = sleepPowerTestArmed || sleepPowerTestRunning;
  bool accountingWasSuspended = intervalAccountingSuspended();

  Serial.println();

  if (!wifiTestWasActive && !sleepTestWasActive) {
    Serial.println("[POWER TEST] No power test was armed or running.");
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

  if (inaRestored) {
    inaShutdownActive = false;
  } else {
    Serial.println(
        "[POWER TEST] ERROR: INA228 is not confirmed to be measuring. "
        "Readings must not be trusted until this is resolved.");
  }

  if (WiFi.getMode() != WIFI_OFF) {
    Serial.println(
        "[POWER TEST] ERROR: Wi-Fi did not turn off. Radio state is unknown.");
  }

  bool persistedStateCleared = true;

  if (wifiPowerTestArmed) {
    if (saveWifiPowerTestArmed(false)) {
      wifiPowerTestArmed = false;
      Serial.println("[POWER TEST] Wi-Fi test flag cleared.");
    } else {
      Serial.println(
          "[POWER TEST] ERROR: Could not clear the persisted Wi-Fi test flag. "
          "It will still be armed after the next reboot.");
      persistedStateCleared = false;
    }
  }

  if (!inaRestored) {
    persistedStateCleared = false;
  }

  if (sleepPowerTestArmed) {
    Serial.print("[POWER TEST] Stopping deep-sleep test variant: ");
    Serial.println(sleepPowerTestVariantName());

    if (saveSleepPowerTestArmed(false)) {
      sleepPowerTestArmed = false;
      Serial.println("[POWER TEST] Deep-sleep test flag cleared.");
    } else {
      Serial.println(
          "[POWER TEST] ERROR: Could not clear the persisted deep-sleep test "
          "flag. It will still be armed after the next reboot.");
      persistedStateCleared = false;
    }

    // Clear the variant selector too, so a later bare POWER TEST SLEEP can
    // never inherit INA-OFF behavior from a test that was already stopped.
    if (sleepPowerTestInaOff) {
      if (saveSleepPowerTestInaOff(false)) {
        sleepPowerTestInaOff = false;
        Serial.println("[POWER TEST] Deep-sleep variant flag cleared.");
      } else {
        Serial.println(
            "[POWER TEST] ERROR: Could not clear the persisted deep-sleep "
            "variant flag.");
        persistedStateCleared = false;
      }
    }
  }

  if (sleepTestWasActive) {
    printSleepTestEvent("SLEEP_TEST_STOPPED", rtcSleepTestCycle);

    Serial.print("[POWER TEST] Completed sleep cycles this power-on: ");
    Serial.println(rtcSleepTestCycle);

    // The counter only describes one continuous battery run, so clear it
    // along with the test rather than letting it leak into the next one.
    rtcSleepTestMagic = 0;
    rtcSleepTestCycle = 0;
  }

  if (accountingWasSuspended) {
    if (!resumeIntervalAccounting()) {
      persistedStateCleared = false;
    }
  }

  if (persistedStateCleared) {
    Serial.println("[POWER TEST] Power test stopped: ALL OK");
    Serial.println("[POWER TEST] Normal continuous logging resumed.");
  } else {
    Serial.println(
        "[POWER TEST] NOT ALL OK - power test stop did not fully succeed.");
  }
}


// ============================================================================
// COMBINED POWER-TEST STATUS
// ============================================================================

void printPowerTestStatus() {
  Serial.println();

  Serial.print("[POWER TEST] Active test: ");

  switch (armedPowerTestMode()) {
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

  if (sleepPowerTestArmed) {
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

  if (inaIsInContinuousMode(false)) {
    Serial.println("continuous conversion");
  } else {
    Serial.println("NOT continuous conversion");
    printPowerTestInaModeDetail();
  }

  if (sleepPowerTestRunning) {
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

  if (intervalAccountingSuspended()) {
    Serial.print("SUSPENDED: ");
    Serial.println(intervalAccountingSuspendReason());
  } else {
    Serial.println("ACTIVE");
  }

  printBootWakeReason();
}


void printHelp() {
  Serial.println();

  Serial.println("[COMMAND] Available commands:");

  Serial.println(
      "  HELP       - show this list");

  Serial.println(
      "  STATUS     - print current experiment state and live sensor reading");

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
      "  RESET      - explain reset confirmation");

  Serial.println(
      "  RESET YES  - start a NEW experiment");

  Serial.println();
}


// ============================================================================
// STATUS COMMAND
// ============================================================================

void printStatus() {
  Serial.println();

  Serial.println("[STATUS] Current logger state:");

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
  if (intervalAccountingSuspended()) {
    Serial.print("[STATUS] Interval accounting = SUSPENDED: ");
    Serial.println(intervalAccountingSuspendReason());
    Serial.println(
        "[STATUS] The interval and totals above are frozen, not advancing.");
  } else {
    Serial.println("[STATUS] Interval accounting = ACTIVE");
  }


  if (inaShutdownActive) {
    Serial.println(
        "[STATUS] INA228               = SHUTDOWN, measurement suspended");
    Serial.println(
        "[STATUS] Voltage/current/power are NOT being updated and are not "
        "reported.");
    return;
  }

  SensorReading reading;

  if (readSensor(reading, false)) {
    Serial.print("[STATUS] Voltage            = ");
    Serial.print(reading.voltage_V, 4);
    Serial.println(" V");

    Serial.print("[STATUS] Current            = ");
    Serial.print(reading.current_mA, 3);
    Serial.println(" mA");

    Serial.print("[STATUS] Power              = ");
    Serial.print(reading.power_mW, 3);
    Serial.println(" mW");
  } else {
    Serial.println(
        "[ERROR] STATUS could not read INA228.");
  }
}


// ============================================================================
// START A NEW EXPERIMENT
// ============================================================================

bool resetExperiment() {
  // A new experiment created while accounting is suspended would be frozen
  // from the moment it was born, and its first interval would never close.
  // Refuse instead of creating an experiment that silently does nothing.
  if (intervalAccountingSuspended()) {
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


  if (experimentId == UINT32_MAX) {
    Serial.println(
        "[FATAL] experiment_id overflow. Cannot safely create another ID.");

    return false;
  }


  // First clear the INA228's physical hardware accumulators.
  //
  // We do this before changing our software totals so an INA failure cannot
  // silently give us a supposedly-clean experiment that actually contains old
  // hardware accumulation.
  if (!resetInaAccumulators()) {
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
  if (!saveCheckpoint()) {
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

void processCommand(String command) {
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


  if (command.length() == 0) {
    return;
  }


  Serial.print("[COMMAND] Received: ");
  Serial.println(command);


  if (command == "HELP") {

    printHelp();

  } else if (command == "STATUS") {

    printStatus();

  } else if (command == "WIFI ON") {

    enableWifi();

  } else if (command == "WIFI OFF") {

    disableWifi();

  } else if (command == "WIFI STATUS") {

    printWifiStatus();

  } else if (command == "POWER TEST WIFI") {

    armWifiPowerTest();

  } else if (command == "POWER TEST SLEEP") {

    armSleepPowerTest(false);

  } else if (command == "POWER TEST SLEEP INA OFF") {

    armSleepPowerTest(true);

  } else if (command == "POWER TEST STOP") {

    stopAllPowerTests();

  } else if (command == "POWER TEST STATUS") {

    printPowerTestStatus();

  } else if (command == "RESET") {

    Serial.println(
        "[COMMAND] RESET requires confirmation.");

    Serial.println(
        "[COMMAND] Type exactly: RESET YES");

  } else if (command == "RESET YES") {

    resetExperiment();

  } else {

    Serial.print("[WARNING] Unknown command: ");
    Serial.println(command);

    Serial.println(
        "[WARNING] Type HELP for available commands.");
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

void handleSerialCommands() {
  // Several characters may have arrived since loop() last ran, so read all
  // currently waiting characters.
  while (Serial.available()) {

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
    if (c == '\n' || c == '\r') {

      // Ignore blank lines.
      if (serialCommandBuffer.length() > 0) {

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
    if (serialCommandBuffer.length() >= 80) {

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

void printHeartbeat() {
  // A shut-down INA228 still answers I2C reads, but the values it returns are
  // left over from before shutdown. Printing them next to "ALL OK" would be
  // presenting stale registers as a live measurement.
  if (inaShutdownActive) {
    Serial.println(
        "[HEARTBEAT] INA228 is in SHUTDOWN. No conversions are running and "
        "no values are being updated.");
    return;
  }

  SensorReading reading;

  if (!readSensor(reading, false)) {
    Serial.println(
        "[HEARTBEAT] ERROR: INA228 read failed");
    return;
  }


  unsigned long elapsedMs =
      millis() - intervalStartMs;


  Serial.print("[HEARTBEAT] ALL OK");

  Serial.print(" | experiment ");
  Serial.print(experimentId);

  if (sleepPowerTestRunning) {
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

  } else if (intervalAccountingSuspended()) {
    // The test is over but accounting could not safely resume. Saying
    // "next interval N" here would promise an interval that will not close.
    Serial.print(" | interval accounting SUSPENDED (");
    Serial.print(intervalAccountingSuspendReason());
    Serial.print(") | ");

  } else {
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

bool closeMeasurementInterval() {
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

  if (!readSensor(reading, false)) {
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

  if (!readAccumulatedCharge_mAh(intervalCharge_mAh)) {
    Serial.println(
        "[ERROR] Could not read accumulated CHARGE.");

    return false;
  }


  Serial.println(
      "[INA228] Reading accumulated ENERGY...");


  double intervalEnergy_mWh;

  if (!readAccumulatedEnergy_mWh(intervalEnergy_mWh)) {
    Serial.println(
        "[ERROR] Could not read accumulated ENERGY.");

    return false;
  }


  Serial.println(
      "[INA228] Closing completed interval.");


  // --------------------------------------------------------------------------
  // Reset INA accumulators so they begin measuring the NEXT interval.
  // --------------------------------------------------------------------------

  if (!resetInaAccumulators()) {
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

  if (!readSensor(diagnosticReading, true)) {
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

  if (!saveCheckpoint()) {
    Serial.println(
        "[ERROR] Measurement succeeded, but NVS checkpoint failed.");

    Serial.println(
        "[ERROR] CSV data above is valid, but power-loss recovery state "
        "is not guaranteed.");
  }


  return true;
}


// ============================================================================
// ARDUINO SETUP()
// ============================================================================
//
// setup() runs ONCE after boot/reset.
// ============================================================================

void setup() {
  // Capture the wake cause before anything else can run. A deep-sleep wake
  // arrives here as an ordinary boot, so this register read is the only way
  // to tell "the user plugged it in" apart from "our sleep timer fired".
  bootWakeupCause = esp_sleep_get_wakeup_cause();

  Serial.begin(115200);

  // Give USB Serial time to enumerate before printing the boot sequence.
  //
  // After a deep-sleep wake the USB device has to re-enumerate on the host,
  // so this delay is doing real work on every wake, not just at power-on.
  delay(1500);

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

  if (!verifyInaIdentity()) {
    Serial.println(
        "[FATAL] INA228 identity check failed. Logger halted.");

    while (true) {
      Serial.println(
          "[FATAL] NOT ALL OK - fix INA228/I2C wiring and reset.");

      delay(5000);
    }
  }


  // --------------------------------------------------------------------------
  // Apply known-good INA228 configuration and READ IT BACK.
  // --------------------------------------------------------------------------

  if (!configureIna228()) {
    Serial.println(
        "[FATAL] INA228 configuration failed. Logger halted.");

    while (true) {
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


  if (!loadCheckpoint()) {
    Serial.println(
        "[FATAL] NVS load failed. Logger halted to avoid corrupting totals.");

    while (true) {
      Serial.println(
          "[FATAL] NOT ALL OK - NVS state requires attention.");

      delay(5000);
    }
  }

  // The flag must survive a USB disconnect and reboot before battery power is
  // applied, so load it independently from the experiment checkpoint.
  if (!loadWifiPowerTestArmed()) {
    Serial.println(
        "[FATAL] Wi-Fi power-test state could not be loaded. Logger halted.");

    while (true) {
      Serial.println(
          "[FATAL] NOT ALL OK - Wi-Fi power-test NVS state requires attention.");

      delay(5000);
    }
  }

  // Same reasoning for the deep-sleep test: the whole point is that arming it
  // over USB survives the USB disconnect and the switch to AA battery power.
  if (!loadSleepPowerTestArmed()) {
    Serial.println(
        "[FATAL] Deep-sleep test state could not be loaded. Logger halted.");

    while (true) {
      Serial.println(
          "[FATAL] NOT ALL OK - deep-sleep test NVS state requires attention.");

      delay(5000);
    }
  }

  // The variant matters as much as the armed flag. Without this load, an
  // armed INA-OFF test would silently resume as the INA-continuous variant
  // after every power cycle, and STATUS would report the wrong one while the
  // DMM measured something else entirely.
  if (!loadSleepPowerTestInaOff()) {
    Serial.println(
        "[FATAL] Deep-sleep test variant could not be loaded. Logger halted.");

    while (true) {
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

  Serial.println(
      "[INA228] Waiting 2 seconds for ADC measurements to settle...");

  delay(2000);

  Serial.println(
      "[INA228] ADC settle complete.");


  // --------------------------------------------------------------------------
  // Take one initial live sensor reading.
  // --------------------------------------------------------------------------

  Serial.println();

  Serial.println(
      "[INA228] Reading current sensor state...");


  SensorReading reading;

  if (!readSensor(reading, true)) {
    Serial.println(
        "[FATAL] Initial INA228 measurement failed. Logger halted.");

    while (true) {
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

  Serial.println(
      "[LOGGER] Preparing measurement interval...");


  if (!resetInaAccumulators()) {
    Serial.println(
        "[FATAL] Could not initialize accumulation interval. Logger halted.");

    while (true) {
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

  PowerTestMode mode = armedPowerTestMode();

  if (mode == POWER_TEST_CONFLICT) {
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

  } else if (mode == POWER_TEST_WIFI) {
    wifiPowerTestRunning = true;
    Serial.println("[POWER TEST] Persisted Wi-Fi power test found.");
    Serial.println("[POWER TEST] Wi-Fi power test ARMED");
    startWifiPowerTestPhase(1);

  } else if (mode == POWER_TEST_SLEEP) {
    Serial.println();
    Serial.println("[POWER TEST] Persisted deep-sleep power test found.");

    bool rtcStateValid = (rtcSleepTestMagic == SLEEP_POWER_TEST_RTC_MAGIC);

    if (bootWakeupCause == ESP_SLEEP_WAKEUP_TIMER) {

      if (rtcStateValid) {
        if (sleepPowerTestInaOff) {
          Serial.println(
              "[POWER TEST] Woke from INA-OFF deep sleep: ALL OK");
        } else {
          Serial.println("[POWER TEST] Woke from deep sleep timer: ALL OK");
        }

        Serial.println("[POWER TEST] Starting next AWAKE phase.");

      } else {
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

    } else {
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
    if (sleepPowerTestInaOff) {
      if (inaIsInContinuousMode(true)) {
        inaShutdownActive = false;
        Serial.println("[INA228] Continuous measurement restored: ALL OK");
      } else {
        Serial.println(
            "[INA228] ERROR: INA228 does not report continuous mode after "
            "wake, even though boot configuration reported success.");

        if (!restoreInaContinuousMode()) {
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

void loop() {
  // Process incoming user commands without waiting/blocking.
  handleSerialCommands();


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
    LIVE_SAMPLE_INTERVAL_MS) {

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
      HEARTBEAT_INTERVAL_MS) {

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
      MEASUREMENT_INTERVAL_MS) {

    if (!closeMeasurementInterval()) {
      Serial.println(
          "[ERROR] Interval close failed.");

      Serial.println(
          "[ERROR] Keeping logger awake for diagnosis.");

      // Do NOT silently pretend a clean new interval began.
      delay(1000);
    }
  }
}