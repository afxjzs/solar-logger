#include <Wire.h>
#include <Preferences.h>
#include <math.h>

// ============================================================================
// BMW SOLAR LOGGER
// Development / NVS Persistence / Shunt Diagnostics
// ============================================================================
//
// DEVELOPMENT RULE:
//
//     WHEN IN DOUBT, PRINT IT OUT.
//
// This version stays fully awake.
// USB Serial remains connected.
//
// New in this version:
//
//     - Prints raw VSHUNT register
//     - Prints signed VSHUNT ADC count
//     - Prints physical shunt voltage in millivolts
//     - Calculates current directly from VSHUNT / RSHUNT
//     - Compares that with the INA228 CURRENT register
//
// ============================================================================


// ============================================================================
// INA228 REGISTER ADDRESSES
// ============================================================================

constexpr uint8_t INA228_ADDRESS = 0x40;

constexpr uint8_t REG_CONFIG     = 0x00;
constexpr uint8_t REG_ADC_CONFIG = 0x01;
constexpr uint8_t REG_SHUNT_CAL  = 0x02;

constexpr uint8_t REG_VSHUNT     = 0x04;  // NEW
constexpr uint8_t REG_VBUS       = 0x05;
constexpr uint8_t REG_DIETEMP    = 0x06;
constexpr uint8_t REG_CURRENT    = 0x07;
constexpr uint8_t REG_POWER      = 0x08;
constexpr uint8_t REG_ENERGY     = 0x09;
constexpr uint8_t REG_CHARGE     = 0x0A;


// ============================================================================
// INA228 CALIBRATION
// ============================================================================

// Effective shunt resistance from our earlier ~16 mA calibration.
//
// R015 nominal resistor:
//     15 mΩ
//
// Our measured effective value:
//     approximately 15.62 mΩ
//
// We are now testing whether this value still makes sense at ~143 mA.
//
constexpr double SHUNT_OHMS = 0.01562;


// CURRENT register scale:
//
//     1 count = 4 microamps
//
constexpr double CURRENT_LSB = 0.000004;


// VSHUNT scale when ADCRANGE = 0:
//
//     1 count = 312.5 nanovolts
//
// 312.5 nV = 0.0000003125 V
//
constexpr double VSHUNT_LSB_VOLTS = 312.5e-9;


// SHUNT_CAL calculated for:
//     CURRENT_LSB = 4 µA
//     SHUNT_OHMS  = 15.62 mΩ
//
constexpr uint16_t SHUNT_CAL_VALUE = 819;


// ============================================================================
// LOGGER TIMING
// ============================================================================

constexpr uint32_t INTERVAL_SECONDS = 60;

constexpr uint32_t HEARTBEAT_SECONDS = 5;

constexpr uint32_t CHECKPOINT_INTERVALS = 5;


// ============================================================================
// NVS
// ============================================================================

constexpr char NVS_NAMESPACE[] = "solarlog";

constexpr uint32_t NVS_SCHEMA_VERSION = 1;

Preferences preferences;


// ============================================================================
// RUNNING STATE
// ============================================================================

uint32_t intervalNumber = 0;

double totalCharge_mAh = 0.0;

double totalEnergy_mWh = 0.0;

uint32_t intervalStartMillis = 0;

uint32_t lastHeartbeatMillis = 0;


// ============================================================================
// PRINT HELPERS
// ============================================================================

void printSeparator() {

  Serial.println(
      "============================================================"
  );
}


void printSmallSeparator() {

  Serial.println(
      "------------------------------------------------------------"
  );
}


// ============================================================================
// READ 16-BIT REGISTER
// ============================================================================

uint16_t readRegister16(uint8_t reg) {

  Wire.beginTransmission(INA228_ADDRESS);

  Wire.write(reg);

  int result =
      Wire.endTransmission(false);


  if (result != 0) {

    Serial.print(
        "[ERROR][I2C] Failed selecting 16-bit register 0x"
    );

    Serial.println(
        reg,
        HEX
    );

    return 0;
  }


  uint8_t received =
      Wire.requestFrom(
          INA228_ADDRESS,
          (uint8_t)2
      );


  if (received != 2) {

    Serial.print(
        "[ERROR][I2C] Expected 2 bytes from register 0x"
    );

    Serial.print(
        reg,
        HEX
    );

    Serial.print(
        ", received "
    );

    Serial.println(
        received
    );

    return 0;
  }


  uint16_t value =
      ((uint16_t)Wire.read() << 8);


  value |=
      (uint16_t)Wire.read();


  return value;
}


// ============================================================================
// WRITE 16-BIT REGISTER
// ============================================================================

bool writeRegister16(
    uint8_t reg,
    uint16_t value
) {

  Wire.beginTransmission(
      INA228_ADDRESS
  );


  Wire.write(
      reg
  );


  // High byte.
  Wire.write(
      (uint8_t)(value >> 8)
  );


  // Low byte.
  Wire.write(
      (uint8_t)(value & 0xFF)
  );


  int result =
      Wire.endTransmission();


  if (result != 0) {

    Serial.print(
        "[ERROR][I2C] Failed writing register 0x"
    );

    Serial.print(
        reg,
        HEX
    );

    Serial.print(
        ". I2C error = "
    );

    Serial.println(
        result
    );

    return false;
  }


  return true;
}


// ============================================================================
// READ 24-BIT REGISTER
// ============================================================================

uint32_t readRegister24(uint8_t reg) {

  Wire.beginTransmission(
      INA228_ADDRESS
  );


  Wire.write(
      reg
  );


  if (
      Wire.endTransmission(false) != 0
  ) {

    Serial.print(
        "[ERROR][I2C] Failed selecting 24-bit register 0x"
    );

    Serial.println(
        reg,
        HEX
    );

    return 0;
  }


  uint8_t received =
      Wire.requestFrom(
          INA228_ADDRESS,
          (uint8_t)3
      );


  if (received != 3) {

    Serial.print(
        "[ERROR][I2C] Expected 3 bytes from register 0x"
    );

    Serial.println(
        reg,
        HEX
    );

    return 0;
  }


  uint32_t value = 0;


  value |=
      ((uint32_t)Wire.read() << 16);


  value |=
      ((uint32_t)Wire.read() << 8);


  value |=
      (uint32_t)Wire.read();


  return value;
}


// ============================================================================
// READ 40-BIT REGISTER
// ============================================================================

uint64_t readRegister40(uint8_t reg) {

  Wire.beginTransmission(
      INA228_ADDRESS
  );


  Wire.write(
      reg
  );


  if (
      Wire.endTransmission(false) != 0
  ) {

    Serial.print(
        "[ERROR][I2C] Failed selecting 40-bit register 0x"
    );

    Serial.println(
        reg,
        HEX
    );

    return 0;
  }


  uint8_t received =
      Wire.requestFrom(
          INA228_ADDRESS,
          (uint8_t)5
      );


  if (received != 5) {

    Serial.print(
        "[ERROR][I2C] Expected 5 bytes from register 0x"
    );

    Serial.println(
        reg,
        HEX
    );

    return 0;
  }


  uint64_t value = 0;


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
// INA228 CURRENT and VSHUNT are signed 20-bit values.
//
// C++ does not have a native 20-bit signed integer.
//
// So we manually turn the 20-bit two's-complement value into an int32_t.
//

int32_t signExtend20(uint32_t value) {

  // Keep only the lower 20 bits.
  value &=
      0xFFFFF;


  // Bit 19 is the sign bit.
  if (
      value & 0x80000
  ) {

    // Fill upper bits with 1 for a negative number.
    value |=
        0xFFF00000;
  }


  return
      (int32_t)value;
}


// CHARGE is signed 40-bit.

int64_t signExtend40(uint64_t value) {

  value &=
      0xFFFFFFFFFFULL;


  if (
      value & 0x8000000000ULL
  ) {

    value |=
        0xFFFFFF0000000000ULL;
  }


  return
      (int64_t)value;
}


// ============================================================================
// INA228 CONNECTION TEST
// ============================================================================

bool testINA228Connection() {

  Serial.println();

  Serial.println(
      "[INA228] Testing I2C communication..."
  );

  Serial.println(
      "[INA228] Sending request to address 0x40..."
  );


  Wire.beginTransmission(
      INA228_ADDRESS
  );


  int result =
      Wire.endTransmission();


  if (result != 0) {

    Serial.print(
        "[ERROR][INA228] Device did not respond. Error = "
    );

    Serial.println(
        result
    );

    return false;
  }


  Serial.println(
      "[INA228] Device responded at 0x40."
  );

  Serial.println(
      "[INA228] Communication: OK"
  );


  return true;
}


// ============================================================================
// INA228 CONFIGURATION
// ============================================================================

bool configureINA228() {

  Serial.println();

  Serial.println(
      "[INA228] Beginning configuration..."
  );


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


  if (
      adcConfig != 0xFB6B
  ) {

    Serial.println(
        "[ERROR][INA228] ADC_CONFIG verification FAILED."
    );

    return false;
  }


  Serial.println(
      "[INA228] ADC_CONFIG verification: OK"
  );


  // --------------------------------------------------------------------------
  // CONFIG
  // --------------------------------------------------------------------------

  uint16_t config =
      readRegister16(
          REG_CONFIG
      );


  Serial.print(
      "[INA228] CONFIG register = 0x"
  );

  Serial.println(
      config,
      HEX
  );


  // Bit 4 is ADCRANGE.
  //
  // We expect:
  //
  //     ADCRANGE = 0
  //
  // which means:
  //
  //     VSHUNT LSB = 312.5 nV/count
  //

  bool adcRange1 =
      config & (1 << 4);


  Serial.print(
      "[INA228] ADCRANGE = "
  );

  Serial.println(
      adcRange1 ? 1 : 0
  );


  if (adcRange1) {

    Serial.println(
        "[ERROR][INA228] ADCRANGE is 1."
    );

    Serial.println(
        "[ERROR][INA228] This firmware expects ADCRANGE = 0."
    );

    return false;
  }


  Serial.println(
      "[INA228] ADCRANGE verification: OK"
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

  Serial.println(
      "..."
  );


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
// RESET ENERGY + CHARGE ACCUMULATORS
// ============================================================================

bool resetAccumulators() {

  Serial.println(
      "[INA228] Resetting ENERGY and CHARGE accumulators..."
  );


  uint16_t config =
      readRegister16(
          REG_CONFIG
      );


  // CONFIG bit 14 = RSTACC.
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
// VSHUNT DIAGNOSTICS
// ============================================================================
//
// This is the new important part.
//
// VSHUNT is a signed 20-bit ADC measurement stored in bits 23:4 of the
// 24-bit register.
//
// Example:
//
//     actual current
//          ↓
//     voltage drop across R015 shunt
//          ↓
//     INA228 ADC
//          ↓
//     VSHUNT raw count
//
// At ADCRANGE = 0:
//
//     one count = 312.5 nanovolts
//
// ============================================================================

int32_t readRawShuntCounts() {

  uint32_t rawRegister =
      readRegister24(
          REG_VSHUNT
      );


  // Useful 20-bit value is bits 23:4.
  uint32_t raw20 =
      rawRegister >> 4;


  return
      signExtend20(
          raw20
      );
}


double readShuntVoltageVolts() {

  int32_t counts =
      readRawShuntCounts();


  return
      counts
      *
      VSHUNT_LSB_VOLTS;
}


double calculateCurrentFromShuntVoltage() {

  double shuntVoltage =
      readShuntVoltageVolts();


  // Ohm's law:
  //
  //     I = V / R
  //
  // Here:
  //
  //     V = physical voltage across the shunt
  //     R = our estimated shunt resistance
  //
  return
      shuntVoltage
      /
      SHUNT_OHMS;
}


// ============================================================================
// OTHER MEASUREMENTS
// ============================================================================

double readBusVoltage() {

  uint32_t rawRegister =
      readRegister24(
          REG_VBUS
      );


  uint32_t rawValue =
      rawRegister >> 4;


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


  uint32_t raw20 =
      rawRegister >> 4;


  int32_t rawSigned =
      signExtend20(
          raw20
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


  double coulombs =
      rawSigned
      *
      CURRENT_LSB;


  double ampHours =
      coulombs
      /
      3600.0;


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


  double joules =
      rawEnergy
      *
      16.0
      *
      3.2
      *
      CURRENT_LSB;


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
// PRINT SHUNT DIAGNOSTICS
// ============================================================================

void printShuntDiagnostics() {

  Serial.println();

  Serial.println(
      "[VSHUNT] Reading raw shunt ADC..."
  );


  // Read the raw 24-bit register once.
  uint32_t rawRegister =
      readRegister24(
          REG_VSHUNT
      );


  // Extract useful bits 23:4.
  uint32_t raw20 =
      rawRegister >> 4;


  // Convert to signed value.
  int32_t signedCounts =
      signExtend20(
          raw20
      );


  // Convert ADC counts to physical voltage.
  double shuntVolts =
      signedCounts
      *
      VSHUNT_LSB_VOLTS;


  double shuntMillivolts =
      shuntVolts
      *
      1000.0;


  // Independently calculate current from:
  //
  //     I = Vshunt / Rshunt
  //
  double shuntCalculatedCurrent =
      shuntVolts
      /
      SHUNT_OHMS;


  // Read INA228 calibrated CURRENT register.
  double inaCurrent =
      readCurrent();


  // Difference between the two calculations.
  double currentDifference_mA =
      (
          inaCurrent
          -
          shuntCalculatedCurrent
      )
      *
      1000.0;


  Serial.print(
      "[VSHUNT] Raw 24-bit register = 0x"
  );

  Serial.println(
      rawRegister,
      HEX
  );


  Serial.print(
      "[VSHUNT] Signed ADC counts    = "
  );

  Serial.println(
      signedCounts
  );


  Serial.print(
      "[VSHUNT] Shunt voltage        = "
  );

  Serial.print(
      shuntMillivolts,
      6
  );

  Serial.println(
      " mV"
  );


  Serial.print(
      "[VSHUNT] Assumed shunt R      = "
  );

  Serial.print(
      SHUNT_OHMS * 1000.0,
      4
  );

  Serial.println(
      " mOhm"
  );


  Serial.print(
      "[VSHUNT] V/R current          = "
  );

  Serial.print(
      shuntCalculatedCurrent * 1000.0,
      3
  );

  Serial.println(
      " mA"
  );


  Serial.print(
      "[CURRENT] INA CURRENT register = "
  );

  Serial.print(
      inaCurrent * 1000.0,
      3
  );

  Serial.println(
      " mA"
  );


  Serial.print(
      "[COMPARE] Difference           = "
  );

  Serial.print(
      currentDifference_mA,
      3
  );

  Serial.println(
      " mA"
  );
}


// ============================================================================
// NVS LOAD
// ============================================================================

void loadCheckpointFromNVS() {

  Serial.println();

  Serial.println(
      "[NVS] Beginning checkpoint load..."
  );


  Serial.print(
      "[NVS] Opening namespace \""
  );

  Serial.print(
      NVS_NAMESPACE
  );

  Serial.println(
      "\" in READ ONLY mode..."
  );


  preferences.begin(
      NVS_NAMESPACE,
      true
  );


  Serial.println(
      "[NVS] Namespace opened."
  );


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


    return;
  }


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
// NVS SAVE
// ============================================================================

void saveCheckpointToNVS() {

  Serial.println();

  printSmallSeparator();


  Serial.println(
      "[NVS] CHECKPOINT REQUIRED"
  );


  Serial.print(
      "[NVS] Writing checkpoint for interval "
  );

  Serial.println(
      intervalNumber
  );


  preferences.begin(
      NVS_NAMESPACE,
      false
  );


  Serial.println(
      "[NVS] Namespace opened."
  );


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

  Serial.println();

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
      "[INA228] Voltage      = "
  );

  Serial.print(
      voltage,
      4
  );

  Serial.println(
      " V"
  );


  Serial.print(
      "[INA228] Current      = "
  );

  Serial.print(
      current * 1000.0,
      3
  );

  Serial.println(
      " mA"
  );


  Serial.print(
      "[INA228] Power        = "
  );

  Serial.print(
      power * 1000.0,
      3
  );

  Serial.println(
      " mW"
  );


  Serial.print(
      "[INA228] Temperature  = "
  );

  Serial.print(
      temperatureC,
      2
  );

  Serial.println(
      " C"
  );


  // NEW:
  printShuntDiagnostics();
}


// ============================================================================
// PROCESS COMPLETED INTERVAL
// ============================================================================

void processCompletedInterval() {

  Serial.println();

  printSeparator();


  Serial.println(
      "[LOGGER] INTERVAL TIMER EXPIRED"
  );


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
  // INSTANTANEOUS DATA
  // --------------------------------------------------------------------------

  Serial.println();

  Serial.println(
      "[INA228] Capturing end-of-interval measurements..."
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
  // SHUNT DATA
  // --------------------------------------------------------------------------

  uint32_t rawShuntRegister =
      readRegister24(
          REG_VSHUNT
      );


  uint32_t rawShunt20 =
      rawShuntRegister >> 4;


  int32_t rawShuntCounts =
      signExtend20(
          rawShunt20
      );


  double shuntVolts =
      rawShuntCounts
      *
      VSHUNT_LSB_VOLTS;


  double shunt_mV =
      shuntVolts
      *
      1000.0;


  double currentFromShunt =
      shuntVolts
      /
      SHUNT_OHMS;

  double shuntPower_mW = current * current * SHUNT_OHMS * 1000.0;
  double shuntRangePercent = fabs(shunt_mV) / 163.84 * 100.0;

  // --------------------------------------------------------------------------
  // ACCUMULATORS
  // --------------------------------------------------------------------------

  Serial.println(
      "[INA228] Reading accumulated CHARGE..."
  );


  double intervalCharge_mAh =
      readCharge_mAh();


  Serial.println(
      "[INA228] Reading accumulated ENERGY..."
  );


  double intervalEnergy_mWh =
      readEnergy_mWh();


  // --------------------------------------------------------------------------
  // RESET ACCUMULATORS FOR NEXT INTERVAL
  // --------------------------------------------------------------------------

  Serial.println(
      "[INA228] Closing completed interval."
  );


  resetAccumulators();


  intervalStartMillis =
      millis();


  Serial.println(
      "[INA228] New accumulation interval started."
  );


  // --------------------------------------------------------------------------
  // UPDATE TOTALS
  // --------------------------------------------------------------------------

  intervalNumber++;


  totalCharge_mAh +=
      intervalCharge_mAh;


  totalEnergy_mWh +=
      intervalEnergy_mWh;


  // --------------------------------------------------------------------------
  // AVERAGES
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
  // SUMMARY
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


  // NEW SHUNT DIAGNOSTIC OUTPUT

  Serial.print(
      "Raw VSHUNT counts:   "
  );

  Serial.println(
      rawShuntCounts
  );


  Serial.print(
      "Shunt voltage:       "
  );

  Serial.print(
      shunt_mV,
      6
  );

  Serial.println(
      " mV"
  );


  Serial.print(
      "VSHUNT/R current:    "
  );

  Serial.print(
      currentFromShunt * 1000.0,
      3
  );

  Serial.println(
      " mA"
  );


  Serial.print(
      "CURRENT register:    "
  );

  Serial.print(
      current * 1000.0,
      3
  );

  Serial.println(
      " mA"
  );


  Serial.print(
      "Current difference:  "
  );

  Serial.print(
      (
          current
          -
          currentFromShunt
      )
      *
      1000.0,
      3
  );

  Serial.println(
      " mA"
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

  Serial.print("Shunt power:         ");
  Serial.print(shuntPower_mW, 3);
  Serial.println(" mW");

  Serial.print("Shunt range used:    ");
  Serial.print(shuntRangePercent, 3);
  Serial.println(" %");

  printSmallSeparator();


  // --------------------------------------------------------------------------
  // NVS CHECKPOINT
  // --------------------------------------------------------------------------

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

    uint32_t remaining =
        CHECKPOINT_INTERVALS
        -
        (
            intervalNumber
            %
            CHECKPOINT_INTERVALS
        );


    Serial.print(
        "[NVS] No checkpoint required. Next checkpoint in "
    );

    Serial.print(
        remaining
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

  Serial.begin(
      115200
  );


  // Give USB Serial time to attach.
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
      "Development + NVS + VSHUNT Diagnostics"
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
  // INA TEST
  // --------------------------------------------------------------------------

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


    printSeparator();


    while (true) {

      Serial.println(
          "[ERROR] Waiting because startup failed."
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
        "INA228 configuration failed."
    );


    printSeparator();


    while (true) {

      Serial.println(
          "[ERROR] Waiting because configuration failed."
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
      "[BOOT] Loading persistent state from NVS..."
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
      "[INA228] ADC settle complete."
  );


  printCurrentMeasurements();


  // --------------------------------------------------------------------------
  // START ACCUMULATION
  // --------------------------------------------------------------------------

  Serial.println();

  Serial.println(
      "[LOGGER] Preparing measurement interval..."
  );


  if (
      !resetAccumulators()
  ) {

    Serial.println(
        "[ERROR] Could not reset accumulators."
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
      "[LOGGER] Measurement interval started."
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
      " intervals"
  );


  Serial.print(
      "Shunt resistance:     "
  );

  Serial.print(
      SHUNT_OHMS * 1000.0,
      4
  );

  Serial.println(
      " mOhm"
  );


  Serial.print(
      "VSHUNT LSB:           "
  );

  Serial.print(
      VSHUNT_LSB_VOLTS * 1e9,
      1
  );

  Serial.println(
      " nV/count"
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

  uint32_t nowMillis =
      millis();


  // --------------------------------------------------------------------------
  // HEARTBEAT
  // --------------------------------------------------------------------------

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
        " | "
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


  // --------------------------------------------------------------------------
  // INTERVAL COMPLETE
  // --------------------------------------------------------------------------

  if (
      nowMillis
      -
      intervalStartMillis
      >=
      INTERVAL_SECONDS * 1000UL
  ) {

    processCompletedInterval();
  }


  delay(
      10
  );
}