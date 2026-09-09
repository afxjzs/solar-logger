#include <Wire.h>
#include <Preferences.h>

// ============================================================================
// BMW SOLAR LOGGER
// Development / NVS Persistence Test
// ============================================================================
//
// PURPOSE:
//
// This version intentionally DOES NOT use deep sleep.
//
// We already proved that:
//
//     ESP32 deep sleep works
//     INA228 stays powered
//     INA228 continues accumulating ENERGY and CHARGE
//
// Right now we want to test something different:
//
//     Can the ESP32 preserve our long-term totals through complete power loss?
//
// Keeping the ESP32 awake also keeps USB Serial connected, which makes this
// version much easier to observe and debug.
//
// DEVELOPMENT RULE:
//
//     WHEN IN DOUBT, PRINT IT OUT.
//
// This sketch prints:
//
//     boot progress
//     setup progress
//     I2C initialization
//     INA228 configuration
//     NVS reads
//     NVS writes
//     periodic heartbeats
//     interval measurements
//     running totals
//     warnings and errors
//
// ============================================================================


// ============================================================================
// INA228 REGISTER ADDRESSES
// ============================================================================

constexpr uint8_t INA228_ADDRESS = 0x40;

constexpr uint8_t REG_CONFIG     = 0x00;
constexpr uint8_t REG_ADC_CONFIG = 0x01;
constexpr uint8_t REG_SHUNT_CAL  = 0x02;
constexpr uint8_t REG_VBUS       = 0x05;
constexpr uint8_t REG_DIETEMP    = 0x06;
constexpr uint8_t REG_CURRENT    = 0x07;
constexpr uint8_t REG_POWER      = 0x08;
constexpr uint8_t REG_ENERGY     = 0x09;
constexpr uint8_t REG_CHARGE     = 0x0A;


// ============================================================================
// INA228 CALIBRATION
// ============================================================================

// Effective shunt resistance that we measured on this board.
//
// The physical resistor is marked:
//
//     R015
//
// which nominally means 15 milliohms.
//
// Our bench calibration gave approximately:
//
//     15.62 milliohms
//
// Therefore:
//
//     15.62 mΩ = 0.01562 Ω
//
constexpr double SHUNT_OHMS = 0.01562;


// Each CURRENT register count represents:
//
//     4 microamps
//
// 4 µA = 0.000004 A
//
constexpr double CURRENT_LSB = 0.000004;


// Our calculated INA228 SHUNT_CAL value.
//
constexpr uint16_t SHUNT_CAL_VALUE = 819;


// ============================================================================
// LOGGER TIMING
// ============================================================================

// Each INA228 accumulation interval will last approximately 60 seconds.
//
// Unlike our deep-sleep version, the ESP32 remains awake during this time.
//
constexpr uint32_t INTERVAL_SECONDS = 60;


// Print a heartbeat every 5 seconds.
//
// This gives us continuous visible proof that the program is still alive.
//
constexpr uint32_t HEARTBEAT_SECONDS = 5;


// Save a durable NVS checkpoint every 5 intervals.
//
// With 60-second intervals:
//
//     5 intervals = approximately 5 minutes.
//
// This is intentionally frequent for the BENCH TEST.
//
// In the finished logger we will probably write less frequently.
//
constexpr uint32_t CHECKPOINT_INTERVALS = 5;


// ============================================================================
// NVS CONFIGURATION
// ============================================================================

// Preferences organizes NVS values into namespaces.
//
// Think of:
//
//     "solarlog"
//
// as the name of a tiny database.
//
constexpr char NVS_NAMESPACE[] = "solarlog";


// Schema/version number.
//
// This lets future firmware detect that the data layout has changed.
//
constexpr uint32_t NVS_SCHEMA_VERSION = 1;


// Preferences object used to access NVS.
//
Preferences preferences;


// ============================================================================
// RUNNING LOGGER STATE
// ============================================================================

// Number of completed measurement intervals.
//
uint32_t intervalNumber = 0;


// Long-term accumulated charge.
//
double totalCharge_mAh = 0.0;


// Long-term accumulated energy.
//
double totalEnergy_mWh = 0.0;


// millis() value corresponding to the beginning of the current INA interval.
//
uint32_t intervalStartMillis = 0;


// millis() value when we last printed a heartbeat.
//
uint32_t lastHeartbeatMillis = 0;


// ============================================================================
// DIAGNOSTIC PRINT HELPERS
// ============================================================================

// Print a visual separator.
//
// This is purely for readability in Serial Monitor.
//
void printSeparator() {

  Serial.println(
      "============================================================"
  );
}


// Print a smaller separator.
//
void printSmallSeparator() {

  Serial.println(
      "------------------------------------------------------------"
  );
}


// ============================================================================
// READ A 16-BIT INA228 REGISTER
// ============================================================================

uint16_t readRegister16(uint8_t reg) {

  Wire.beginTransmission(INA228_ADDRESS);

  Wire.write(reg);

  int transmissionResult =
      Wire.endTransmission(false);

  if (transmissionResult != 0) {

    Serial.print("[ERROR][I2C] Failed selecting register 0x");
    Serial.println(reg, HEX);

    return 0;
  }


  uint8_t bytesReceived =
      Wire.requestFrom(
          INA228_ADDRESS,
          (uint8_t)2
      );

  if (bytesReceived != 2) {

    Serial.print("[ERROR][I2C] Expected 2 bytes from register 0x");
    Serial.print(reg, HEX);

    Serial.print(", received ");
    Serial.println(bytesReceived);

    return 0;
  }


  // First byte is the upper 8 bits.
  uint16_t value =
      ((uint16_t)Wire.read() << 8);

  // Second byte is the lower 8 bits.
  value |=
      (uint16_t)Wire.read();

  return value;
}


// ============================================================================
// WRITE A 16-BIT INA228 REGISTER
// ============================================================================

bool writeRegister16(
    uint8_t reg,
    uint16_t value
) {

  Wire.beginTransmission(INA228_ADDRESS);

  Wire.write(reg);

  // Write high byte.
  Wire.write(
      (uint8_t)(value >> 8)
  );

  // Write low byte.
  Wire.write(
      (uint8_t)(value & 0xFF)
  );


  int result =
      Wire.endTransmission();


  if (result != 0) {

    Serial.print("[ERROR][I2C] Failed writing register 0x");
    Serial.print(reg, HEX);

    Serial.print(". I2C error = ");
    Serial.println(result);

    return false;
  }


  return true;
}


// ============================================================================
// READ A 24-BIT INA228 REGISTER
// ============================================================================

uint32_t readRegister24(uint8_t reg) {

  Wire.beginTransmission(INA228_ADDRESS);

  Wire.write(reg);


  if (Wire.endTransmission(false) != 0) {

    Serial.print("[ERROR][I2C] Failed selecting 24-bit register 0x");
    Serial.println(reg, HEX);

    return 0;
  }


  uint8_t bytesReceived =
      Wire.requestFrom(
          INA228_ADDRESS,
          (uint8_t)3
      );


  if (bytesReceived != 3) {

    Serial.print("[ERROR][I2C] Expected 3 bytes from register 0x");
    Serial.println(reg, HEX);

    return 0;
  }


  uint32_t value = 0;


  // First byte becomes bits 23:16.
  value |=
      ((uint32_t)Wire.read() << 16);


  // Second byte becomes bits 15:8.
  value |=
      ((uint32_t)Wire.read() << 8);


  // Third byte becomes bits 7:0.
  value |=
      (uint32_t)Wire.read();


  return value;
}


// ============================================================================
// READ A 40-BIT INA228 REGISTER
// ============================================================================

uint64_t readRegister40(uint8_t reg) {

  Wire.beginTransmission(INA228_ADDRESS);

  Wire.write(reg);


  if (Wire.endTransmission(false) != 0) {

    Serial.print("[ERROR][I2C] Failed selecting 40-bit register 0x");
    Serial.println(reg, HEX);

    return 0;
  }


  uint8_t bytesReceived =
      Wire.requestFrom(
          INA228_ADDRESS,
          (uint8_t)5
      );


  if (bytesReceived != 5) {

    Serial.print("[ERROR][I2C] Expected 5 bytes from register 0x");
    Serial.println(reg, HEX);

    return 0;
  }


  uint64_t value = 0;


  // A 40-bit register contains five bytes.
  //
  // Each pass:
  //
  //     move our previous bits left by 8
  //     insert the new byte at the bottom
  //
  for (int i = 0; i < 5; i++) {

    value =
        (value << 8)
        |
        Wire.read();
  }


  return value;
}


// ============================================================================
// SIGN EXTENSION
// ============================================================================
//
// CURRENT is signed 20-bit.
//
// ESP32 C++ does not have an int20_t.
//
// We therefore convert it into a normal signed 32-bit integer.
//

int32_t signExtend20(uint32_t value) {

  // Keep only 20 bits.
  value &=
      0xFFFFF;


  // Bit 19 is the sign bit.
  //
  // If it is 1, this is a negative number.
  if (value & 0x80000) {

    value |=
        0xFFF00000;
  }


  return
      (int32_t)value;
}


// CHARGE is signed 40-bit.
//
// Same concept, but we store the result in int64_t.
//
int64_t signExtend40(uint64_t value) {

  value &=
      0xFFFFFFFFFFULL;


  // Bit 39 is the sign bit.
  if (value & 0x8000000000ULL) {

    value |=
        0xFFFFFF0000000000ULL;
  }


  return
      (int64_t)value;
}


// ============================================================================
// INA228 COMMUNICATION TEST
// ============================================================================

bool testINA228Connection() {

  Serial.println("[INA228] Testing I2C communication...");
  Serial.println("[INA228] Sending request to address 0x40...");


  Wire.beginTransmission(
      INA228_ADDRESS
  );


  int result =
      Wire.endTransmission();


  if (result != 0) {

    Serial.print("[ERROR][INA228] Device did not respond. I2C error = ");
    Serial.println(result);

    return false;
  }


  Serial.println("[INA228] Device responded at 0x40.");
  Serial.println("[INA228] Communication: OK");

  return true;
}


// ============================================================================
// CONFIGURE INA228
// ============================================================================

bool configureINA228() {

  Serial.println();
  Serial.println("[INA228] Beginning configuration...");


  // --------------------------------------------------------------------------
  // ADC CONFIG
  // --------------------------------------------------------------------------

  Serial.println(
      "[INA228] Writing ADC_CONFIG = 0xFB6B..."
  );


  if (
      !writeRegister16(
          REG_ADC_CONFIG,
          0xFB6B
      )
  ) {

    Serial.println(
        "[ERROR][INA228] Failed to write ADC_CONFIG."
    );

    return false;
  }


  // Read it back so we can verify the write.
  uint16_t adcConfig =
      readRegister16(
          REG_ADC_CONFIG
      );


  Serial.print(
      "[INA228] ADC_CONFIG readback = 0x"
  );

  Serial.println(
      adcConfig,
      HEX
  );


  if (adcConfig != 0xFB6B) {

    Serial.println(
        "[ERROR][INA228] ADC_CONFIG verification FAILED."
    );

    return false;
  }


  Serial.println(
      "[INA228] ADC_CONFIG verification: OK"
  );


  // --------------------------------------------------------------------------
  // SHUNT CALIBRATION
  // --------------------------------------------------------------------------

  Serial.print(
      "[INA228] Writing SHUNT_CAL = "
  );

  Serial.print(
      SHUNT_CAL_VALUE
  );

  Serial.println("...");


  if (
      !writeRegister16(
          REG_SHUNT_CAL,
          SHUNT_CAL_VALUE
      )
  ) {

    Serial.println(
        "[ERROR][INA228] Failed to write SHUNT_CAL."
    );

    return false;
  }


  uint16_t shuntCal =
      readRegister16(
          REG_SHUNT_CAL
      );


  Serial.print(
      "[INA228] SHUNT_CAL readback = "
  );

  Serial.println(
      shuntCal
  );


  if (
      shuntCal != SHUNT_CAL_VALUE
  ) {

    Serial.println(
        "[ERROR][INA228] SHUNT_CAL verification FAILED."
    );

    return false;
  }


  Serial.println(
      "[INA228] SHUNT_CAL verification: OK"
  );


  Serial.println(
      "[INA228] Configuration complete."
  );


  return true;
}


// ============================================================================
// RESET INA228 ENERGY + CHARGE ACCUMULATORS
// ============================================================================

bool resetAccumulators() {

  Serial.println(
      "[INA228] Resetting ENERGY and CHARGE accumulators..."
  );


  uint16_t config =
      readRegister16(
          REG_CONFIG
      );


  // CONFIG bit 14 is RSTACC.
  //
  // Setting this bit tells the INA228 to clear:
  //
  //     ENERGY
  //     CHARGE
  //
  config |=
      (1 << 14);


  if (
      !writeRegister16(
          REG_CONFIG,
          config
      )
  ) {

    Serial.println(
        "[ERROR][INA228] Accumulator reset FAILED."
    );

    return false;
  }


  Serial.println(
      "[INA228] Accumulators reset: OK"
  );


  return true;
}


// ============================================================================
// MEASUREMENT FUNCTIONS
// ============================================================================

double readBusVoltage() {

  uint32_t rawRegister =
      readRegister24(
          REG_VBUS
      );


  // VBUS measurement occupies bits 23:4.
  uint32_t rawValue =
      rawRegister >> 4;


  // Each count represents:
  //
  //     195.3125 microvolts
  //
  return
      rawValue
      *
      195.3125e-6;
}


double readCurrent() {

  uint32_t rawRegister =
      readRegister24(
          REG_CURRENT
      );


  uint32_t rawUnsigned =
      rawRegister >> 4;


  int32_t rawSigned =
      signExtend20(
          rawUnsigned
      );


  return
      rawSigned
      *
      CURRENT_LSB;
}


double readPower() {

  uint32_t rawPower =
      readRegister24(
          REG_POWER
      );


  return
      rawPower
      *
      3.2
      *
      CURRENT_LSB;
}


double readTemperatureC() {

  int16_t rawTemperature =
      (int16_t)
      readRegister16(
          REG_DIETEMP
      );


  return
      rawTemperature
      *
      0.0078125;
}


double readCharge_mAh() {

  uint64_t rawUnsigned =
      readRegister40(
          REG_CHARGE
      );


  int64_t rawSigned =
      signExtend40(
          rawUnsigned
      );


  // INA228 CHARGE scaling gives coulombs.
  double coulombs =
      rawSigned
      *
      CURRENT_LSB;


  // 3600 coulombs = 1 amp-hour.
  double ampHours =
      coulombs
      /
      3600.0;


  // Convert Ah to mAh.
  return
      ampHours
      *
      1000.0;
}


double readEnergy_mWh() {

  uint64_t rawEnergy =
      readRegister40(
          REG_ENERGY
      );


  // INA228 ENERGY scaling gives joules.
  double joules =
      rawEnergy
      *
      16.0
      *
      3.2
      *
      CURRENT_LSB;


  // Convert joules to watt-hours.
  double wattHours =
      joules
      /
      3600.0;


  return
      wattHours
      *
      1000.0;
}


// ============================================================================
// NVS: LOAD CHECKPOINT
// ============================================================================

void loadCheckpointFromNVS() {

  Serial.println();
  Serial.println("[NVS] Beginning checkpoint load...");

  Serial.print("[NVS] Opening namespace \"");
  Serial.print(NVS_NAMESPACE);
  Serial.println("\" in READ ONLY mode...");


  preferences.begin(
      NVS_NAMESPACE,
      true
  );


  Serial.println("[NVS] Namespace opened.");


  // --------------------------------------------------------------------------
  // CHECK SCHEMA VERSION
  // --------------------------------------------------------------------------

  Serial.println(
      "[NVS] Reading schema version..."
  );


  uint32_t savedSchema =
      preferences.getUInt(
          "schema",
          0
      );


  Serial.print(
      "[NVS] Stored schema = "
  );

  Serial.println(
      savedSchema
  );


  // If schema doesn't match, there is no valid checkpoint for this firmware.
  if (
      savedSchema != NVS_SCHEMA_VERSION
  ) {

    Serial.println(
        "[NVS] No compatible checkpoint found."
    );

    Serial.println(
        "[NVS] Starting totals at zero."
    );


    intervalNumber = 0;
    totalCharge_mAh = 0.0;
    totalEnergy_mWh = 0.0;


    preferences.end();


    Serial.println(
        "[NVS] Namespace closed."
    );


    return;
  }


  // --------------------------------------------------------------------------
  // LOAD INTERVAL NUMBER
  // --------------------------------------------------------------------------

  Serial.println(
      "[NVS] Reading saved interval..."
  );


  intervalNumber =
      preferences.getUInt(
          "interval",
          0
      );


  Serial.print(
      "[NVS] interval = "
  );

  Serial.println(
      intervalNumber
  );


  // --------------------------------------------------------------------------
  // LOAD CHARGE
  // --------------------------------------------------------------------------

  Serial.println(
      "[NVS] Reading saved charge..."
  );


  totalCharge_mAh =
      preferences.getDouble(
          "charge",
          0.0
      );


  Serial.print(
      "[NVS] charge = "
  );

  Serial.print(
      totalCharge_mAh,
      6
  );

  Serial.println(
      " mAh"
  );


  // --------------------------------------------------------------------------
  // LOAD ENERGY
  // --------------------------------------------------------------------------

  Serial.println(
      "[NVS] Reading saved energy..."
  );


  totalEnergy_mWh =
      preferences.getDouble(
          "energy",
          0.0
      );


  Serial.print(
      "[NVS] energy = "
  );

  Serial.print(
      totalEnergy_mWh,
      6
  );

  Serial.println(
      " mWh"
  );


  preferences.end();


  Serial.println(
      "[NVS] Namespace closed."
  );


  Serial.println(
      "[NVS] Checkpoint load: OK"
  );
}


// ============================================================================
// NVS: SAVE CHECKPOINT
// ============================================================================

void saveCheckpointToNVS() {

  Serial.println();
  printSmallSeparator();

  Serial.println(
      "[NVS] CHECKPOINT REQUIRED"
  );

  Serial.print(
      "[NVS] Interval "
  );

  Serial.print(
      intervalNumber
  );

  Serial.print(
      " is divisible by "
  );

  Serial.println(
      CHECKPOINT_INTERVALS
  );


  Serial.print(
      "[NVS] Opening namespace \""
  );

  Serial.print(
      NVS_NAMESPACE
  );

  Serial.println(
      "\" in READ/WRITE mode..."
  );


  preferences.begin(
      NVS_NAMESPACE,
      false
  );


  Serial.println(
      "[NVS] Namespace opened."
  );


  // --------------------------------------------------------------------------
  // WRITE SCHEMA
  // --------------------------------------------------------------------------

  Serial.print(
      "[NVS] Writing schema = "
  );

  Serial.println(
      NVS_SCHEMA_VERSION
  );


  preferences.putUInt(
      "schema",
      NVS_SCHEMA_VERSION
  );


  // --------------------------------------------------------------------------
  // WRITE INTERVAL
  // --------------------------------------------------------------------------

  Serial.print(
      "[NVS] Writing interval = "
  );

  Serial.println(
      intervalNumber
  );


  preferences.putUInt(
      "interval",
      intervalNumber
  );


  // --------------------------------------------------------------------------
  // WRITE CHARGE
  // --------------------------------------------------------------------------

  Serial.print(
      "[NVS] Writing charge = "
  );

  Serial.print(
      totalCharge_mAh,
      6
  );

  Serial.println(
      " mAh"
  );


  preferences.putDouble(
      "charge",
      totalCharge_mAh
  );


  // --------------------------------------------------------------------------
  // WRITE ENERGY
  // --------------------------------------------------------------------------

  Serial.print(
      "[NVS] Writing energy = "
  );

  Serial.print(
      totalEnergy_mWh,
      6
  );

  Serial.println(
      " mWh"
  );


  preferences.putDouble(
      "energy",
      totalEnergy_mWh
  );


  Serial.println(
      "[NVS] Closing namespace..."
  );


  preferences.end();


  Serial.println(
      "[NVS] Namespace closed."
  );


  Serial.println();
  Serial.println(
      "*** NVS CHECKPOINT SAVED SUCCESSFULLY ***"
  );

  printSmallSeparator();
}


// ============================================================================
// PRINT CURRENT SENSOR STATE
// ============================================================================

void printCurrentMeasurements() {

  Serial.println(
      "[INA228] Reading current sensor state..."
  );


  double voltage =
      readBusVoltage();


  double current =
      readCurrent();


  double power =
      readPower();


  double temperatureC =
      readTemperatureC();


  Serial.print(
      "[INA228] Voltage     = "
  );

  Serial.print(
      voltage,
      4
  );

  Serial.println(
      " V"
  );


  Serial.print(
      "[INA228] Current     = "
  );

  Serial.print(
      current * 1000.0,
      3
  );

  Serial.println(
      " mA"
  );


  Serial.print(
      "[INA228] Power       = "
  );

  Serial.print(
      power * 1000.0,
      3
  );

  Serial.println(
      " mW"
  );


  Serial.print(
      "[INA228] Temperature = "
  );

  Serial.print(
      temperatureC,
      2
  );

  Serial.println(
      " C"
  );
}


// ============================================================================
// PROCESS ONE COMPLETE INTERVAL
// ============================================================================

void processCompletedInterval() {

  Serial.println();
  printSeparator();

  Serial.println(
      "[LOGGER] INTERVAL TIMER EXPIRED"
  );


  // Capture actual elapsed time.
  //
  // This is more accurate than assuming the interval lasted EXACTLY 60 seconds.
  uint32_t nowMillis =
      millis();


  uint32_t elapsedMillis =
      nowMillis
      -
      intervalStartMillis;


  double elapsedHours =
      elapsedMillis
      /
      3600000.0;


  Serial.print(
      "[LOGGER] Actual elapsed time = "
  );

  Serial.print(
      elapsedMillis / 1000.0,
      3
  );

  Serial.println(
      " seconds"
  );


  // --------------------------------------------------------------------------
  // READ INSTANTANEOUS VALUES
  // --------------------------------------------------------------------------

  Serial.println(
      "[INA228] Reading instantaneous measurements..."
  );


  double voltage =
      readBusVoltage();


  double current =
      readCurrent();


  double power =
      readPower();


  double temperatureC =
      readTemperatureC();


  // --------------------------------------------------------------------------
  // READ ACCUMULATORS
  // --------------------------------------------------------------------------

  Serial.println(
      "[INA228] Reading accumulated CHARGE..."
  );


  double intervalCharge_mAh =
      readCharge_mAh();


  Serial.print(
      "[INA228] Interval charge = "
  );

  Serial.print(
      intervalCharge_mAh,
      6
  );

  Serial.println(
      " mAh"
  );


  Serial.println(
      "[INA228] Reading accumulated ENERGY..."
  );


  double intervalEnergy_mWh =
      readEnergy_mWh();


  Serial.print(
      "[INA228] Interval energy = "
  );

  Serial.print(
      intervalEnergy_mWh,
      6
  );

  Serial.println(
      " mWh"
  );


  // --------------------------------------------------------------------------
  // IMMEDIATELY START NEXT INA INTERVAL
  // --------------------------------------------------------------------------
  //
  // We do this BEFORE all the printing below.
  //
  // Why?
  //
  // We don't want the time spent printing to Serial to become dead time.
  //
  // Reset the INA now, and let it begin accumulating the next interval while
  // the ESP32 prints and writes NVS.

  Serial.println(
      "[INA228] Closing completed interval."
  );


  resetAccumulators();


  intervalStartMillis =
      millis();


  Serial.println(
      "[INA228] New accumulation interval has started."
  );


  // --------------------------------------------------------------------------
  // UPDATE COUNTERS
  // --------------------------------------------------------------------------

  intervalNumber++;


  Serial.print(
      "[LOGGER] Incremented interval number to "
  );

  Serial.println(
      intervalNumber
  );


  Serial.println(
      "[LOGGER] Adding interval charge to running total..."
  );


  totalCharge_mAh +=
      intervalCharge_mAh;


  Serial.println(
      "[LOGGER] Adding interval energy to running total..."
  );


  totalEnergy_mWh +=
      intervalEnergy_mWh;


  // --------------------------------------------------------------------------
  // CALCULATE INTERVAL AVERAGES
  // --------------------------------------------------------------------------

  double averageCurrent_mA =
      intervalCharge_mAh
      /
      elapsedHours;


  double averagePower_mW =
      intervalEnergy_mWh
      /
      elapsedHours;


  // --------------------------------------------------------------------------
  // PRINT INTERVAL SUMMARY
  // --------------------------------------------------------------------------

  Serial.println();

  printSmallSeparator();

  Serial.print(
      "INTERVAL #"
  );

  Serial.println(
      intervalNumber
  );


  Serial.print(
      "Actual interval:     "
  );

  Serial.print(
      elapsedMillis / 1000.0,
      3
  );

  Serial.println(
      " seconds"
  );


  Serial.println();


  Serial.print(
      "Ending voltage:      "
  );

  Serial.print(
      voltage,
      4
  );

  Serial.println(
      " V"
  );


  Serial.print(
      "Current right now:   "
  );

  Serial.print(
      current * 1000.0,
      3
  );

  Serial.println(
      " mA"
  );


  Serial.print(
      "Power right now:     "
  );

  Serial.print(
      power * 1000.0,
      3
  );

  Serial.println(
      " mW"
  );


  Serial.print(
      "Temperature:         "
  );

  Serial.print(
      temperatureC,
      2
  );

  Serial.println(
      " C"
  );


  Serial.println();


  Serial.print(
      "Interval charge:     "
  );

  Serial.print(
      intervalCharge_mAh,
      6
  );

  Serial.println(
      " mAh"
  );


  Serial.print(
      "Interval energy:     "
  );

  Serial.print(
      intervalEnergy_mWh,
      6
  );

  Serial.println(
      " mWh"
  );


  Serial.print(
      "Average current:     "
  );

  Serial.print(
      averageCurrent_mA,
      3
  );

  Serial.println(
      " mA"
  );


  Serial.print(
      "Average power:       "
  );

  Serial.print(
      averagePower_mW,
      3
  );

  Serial.println(
      " mW"
  );


  Serial.println();


  Serial.print(
      "RUNNING charge:      "
  );

  Serial.print(
      totalCharge_mAh,
      6
  );

  Serial.println(
      " mAh"
  );


  Serial.print(
      "RUNNING energy:      "
  );

  Serial.print(
      totalEnergy_mWh,
      6
  );

  Serial.println(
      " mWh"
  );


  printSmallSeparator();


  // --------------------------------------------------------------------------
  // DETERMINE WHETHER NVS CHECKPOINT IS DUE
  // --------------------------------------------------------------------------

  Serial.println();


  uint32_t intervalsUntilCheckpoint =
      CHECKPOINT_INTERVALS
      -
      (
          intervalNumber
          %
          CHECKPOINT_INTERVALS
      );


  if (
      intervalNumber
      %
      CHECKPOINT_INTERVALS
      ==
      0
  ) {

    saveCheckpointToNVS();
  }

  else {

    Serial.print(
        "[NVS] No checkpoint required. Next checkpoint in "
    );

    Serial.print(
        intervalsUntilCheckpoint
    );

    Serial.println(
        " interval(s)."
    );
  }


  Serial.println();

  Serial.println(
      "[LOGGER] Interval processing complete."
  );


  Serial.println(
      "[LOGGER] Returning to loop()."
  );


  printSeparator();
}


// ============================================================================
// SETUP
// ============================================================================

void setup() {

  // --------------------------------------------------------------------------
  // SERIAL
  // --------------------------------------------------------------------------

  Serial.begin(
      115200
  );


  // Give macOS and Arduino Serial Monitor time to attach.
  //
  // Unlike the deep-sleep version, the USB port will remain alive afterward.
  delay(
      2000
  );


  Serial.println();
  Serial.println();

  printSeparator();

  Serial.println(
      "BMW SOLAR LOGGER"
  );

  Serial.println(
      "Development + NVS Persistence Test"
  );

  printSeparator();

  Serial.println();

  Serial.println(
      "[BOOT] Firmware started."
  );

  Serial.println(
      "[BOOT] Entering setup()."
  );


  // --------------------------------------------------------------------------
  // I2C
  // --------------------------------------------------------------------------

  Serial.println();

  Serial.println(
      "[I2C] Starting I2C interface..."
  );


  Serial.println(
      "[I2C] XIAO D4 = SDA"
  );


  Serial.println(
      "[I2C] XIAO D5 = SCL"
  );


  Wire.begin(
      D4,
      D5
  );


  Serial.println(
      "[I2C] Wire.begin() completed."
  );


  // --------------------------------------------------------------------------
  // INA CONNECTION TEST
  // --------------------------------------------------------------------------

  Serial.println();

  if (
      !testINA228Connection()
  ) {

    printSeparator();

    Serial.println(
        "STARTUP FAILED"
    );

    Serial.println(
        "INA228 could not be reached."
    );

    Serial.println(
        "Program will remain awake so you can diagnose it."
    );

    printSeparator();


    // Stay here forever.
    //
    // Importantly, we do NOT silently reboot or sleep.
    while (true) {

      Serial.println(
          "[ERROR] Waiting here because startup failed."
      );

      delay(
          5000
      );
    }
  }


  // --------------------------------------------------------------------------
  // INA CONFIGURATION
  // --------------------------------------------------------------------------

  if (
      !configureINA228()
  ) {

    printSeparator();

    Serial.println(
        "STARTUP FAILED"
    );

    Serial.println(
        "INA228 configuration could not be verified."
    );

    printSeparator();


    while (true) {

      Serial.println(
          "[ERROR] Waiting here because configuration failed."
      );

      delay(
          5000
      );
    }
  }


  // --------------------------------------------------------------------------
  // NVS
  // --------------------------------------------------------------------------

  Serial.println();

  Serial.println(
      "[BOOT] INA228 configuration succeeded."
  );


  Serial.println(
      "[BOOT] Loading durable logger state from NVS..."
  );


  loadCheckpointFromNVS();


  // --------------------------------------------------------------------------
  // SENSOR SETTLE
  // --------------------------------------------------------------------------

  Serial.println();

  Serial.println(
      "[INA228] Waiting 2 seconds for ADC measurements to settle..."
  );


  delay(
      2000
  );


  Serial.println(
      "[INA228] Settle period complete."
  );


  // Show what we're measuring right now.
  printCurrentMeasurements();


  // --------------------------------------------------------------------------
  // BEGIN FIRST INTERVAL
  // --------------------------------------------------------------------------

  Serial.println();

  Serial.println(
      "[LOGGER] Preparing first measurement interval..."
  );


  if (
      !resetAccumulators()
  ) {

    Serial.println(
        "[ERROR] Could not start first accumulation interval."
    );


    while (true) {

      Serial.println(
          "[ERROR] Logger halted."
      );

      delay(
          5000
      );
    }
  }


  intervalStartMillis =
      millis();


  lastHeartbeatMillis =
      millis();


  Serial.println(
      "[LOGGER] First measurement interval has started."
  );


  // --------------------------------------------------------------------------
  // ALL OK
  // --------------------------------------------------------------------------

  Serial.println();

  printSeparator();

  Serial.println(
      "ALL OK"
  );

  Serial.println(
      "Initialization completed successfully."
  );

  Serial.println();
  Serial.println(
      "USB Serial will remain connected."
  );

  Serial.print(
      "Measurement interval: "
  );

  Serial.print(
      INTERVAL_SECONDS
  );

  Serial.println(
      " seconds"
  );


  Serial.print(
      "Heartbeat interval:   "
  );

  Serial.print(
      HEARTBEAT_SECONDS
  );

  Serial.println(
      " seconds"
  );


  Serial.print(
      "NVS checkpoint every: "
  );

  Serial.print(
      CHECKPOINT_INTERVALS
  );

  Serial.println(
      " measurement intervals"
  );


  Serial.println();

  Serial.print(
      "Restored interval:    "
  );

  Serial.println(
      intervalNumber
  );


  Serial.print(
      "Restored charge:      "
  );

  Serial.print(
      totalCharge_mAh,
      6
  );

  Serial.println(
      " mAh"
  );


  Serial.print(
      "Restored energy:      "
  );

  Serial.print(
      totalEnergy_mWh,
      6
  );

  Serial.println(
      " mWh"
  );


  printSeparator();

  Serial.println();

  Serial.println(
      "[BOOT] Leaving setup()."
  );

  Serial.println(
      "[BOOT] Arduino will now call loop() repeatedly."
  );
}


// ============================================================================
// LOOP
// ============================================================================

void loop() {

  // loop() executes over and over, usually thousands of times per second.
  //
  // Therefore we DO NOT print every time loop() executes.
  //
  // If we did, Serial would be completely flooded.
  //
  // Instead we print a heartbeat every 5 seconds.


  uint32_t nowMillis =
      millis();


  // ==========================================================================
  // HEARTBEAT
  // ==========================================================================

  if (
      nowMillis
      -
      lastHeartbeatMillis
      >=
      HEARTBEAT_SECONDS * 1000UL
  ) {

    lastHeartbeatMillis =
        nowMillis;


    uint32_t elapsedSeconds =
        (
            nowMillis
            -
            intervalStartMillis
        )
        /
        1000;


    uint32_t secondsRemaining;

    if (
        elapsedSeconds
        >=
        INTERVAL_SECONDS
    ) {

      secondsRemaining = 0;
    }

    else {

      secondsRemaining =
          INTERVAL_SECONDS
          -
          elapsedSeconds;
    }


    Serial.print(
        "[HEARTBEAT] ALL OK | interval "
    );

    Serial.print(
        intervalNumber + 1
    );

    Serial.print(
        " in progress | "
    );

    Serial.print(
        elapsedSeconds
    );

    Serial.print(
        " sec elapsed | "
    );

    Serial.print(
        secondsRemaining
    );

    Serial.println(
        " sec remaining"
    );
  }


  // ==========================================================================
  // INTERVAL TIMER
  // ==========================================================================

  if (
      nowMillis
      -
      intervalStartMillis
      >=
      INTERVAL_SECONDS * 1000UL
  ) {

    processCompletedInterval();
  }


  // Tiny delay so we don't spin the CPU pointlessly at maximum speed.
  //
  // 10 milliseconds is short enough that it has no meaningful effect on
  // our one-minute timing.
  delay(
      10
  );
}