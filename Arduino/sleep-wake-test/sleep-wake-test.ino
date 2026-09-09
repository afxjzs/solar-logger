#include <Wire.h>
#include "esp_sleep.h"

// ============================================================================
// INA228 REGISTER ADDRESSES
// ============================================================================

constexpr uint8_t INA228_ADDRESS = 0x40;

constexpr uint8_t REG_CONFIG     = 0x00;
constexpr uint8_t REG_ADC_CONFIG = 0x01;
constexpr uint8_t REG_SHUNT_CAL  = 0x02;
constexpr uint8_t REG_VBUS       = 0x05;
constexpr uint8_t REG_CURRENT    = 0x07;
constexpr uint8_t REG_POWER      = 0x08;
constexpr uint8_t REG_ENERGY     = 0x09;
constexpr uint8_t REG_CHARGE     = 0x0A;


// ============================================================================
// CALIBRATION VALUES
// ============================================================================

// Our measured effective shunt resistance.
constexpr double SHUNT_OHMS = 0.01562;

// Each CURRENT register count represents 4 microamps.
constexpr double CURRENT_LSB = 0.000004;

// Calibration value calculated earlier.
constexpr uint16_t SHUNT_CAL_VALUE = 819;


// ============================================================================
// SLEEP TEST SETTINGS
// ============================================================================

// We will sleep for exactly 60 seconds.
//
// Later, the real logger may sleep for:
//     15 minutes
//     30 minutes
//     1 hour
//
// But 60 seconds is ideal for a bench test.
constexpr uint64_t SLEEP_SECONDS = 7200;


// ============================================================================
// READ A 16-BIT INA228 REGISTER
// ============================================================================

uint16_t readRegister16(uint8_t reg) {

  Wire.beginTransmission(INA228_ADDRESS);

  Wire.write(reg);

  if (Wire.endTransmission(false) != 0) {
    Serial.println("ERROR: could not select 16-bit register");
    return 0;
  }

  if (Wire.requestFrom(INA228_ADDRESS, (uint8_t)2) != 2) {
    Serial.println("ERROR: expected 2 bytes");
    return 0;
  }

  uint16_t value = ((uint16_t)Wire.read() << 8);

  value |= (uint16_t)Wire.read();

  return value;
}


// ============================================================================
// WRITE A 16-BIT INA228 REGISTER
// ============================================================================

void writeRegister16(uint8_t reg, uint16_t value) {

  Wire.beginTransmission(INA228_ADDRESS);

  Wire.write(reg);

  // Send the upper byte.
  Wire.write((uint8_t)(value >> 8));

  // Send the lower byte.
  Wire.write((uint8_t)(value & 0xFF));

  Wire.endTransmission();
}


// ============================================================================
// READ A 24-BIT INA228 REGISTER
// ============================================================================

uint32_t readRegister24(uint8_t reg) {

  Wire.beginTransmission(INA228_ADDRESS);

  Wire.write(reg);

  if (Wire.endTransmission(false) != 0) {
    Serial.println("ERROR: could not select 24-bit register");
    return 0;
  }

  if (Wire.requestFrom(INA228_ADDRESS, (uint8_t)3) != 3) {
    Serial.println("ERROR: expected 3 bytes");
    return 0;
  }

  uint32_t value = 0;

  value |= ((uint32_t)Wire.read() << 16);
  value |= ((uint32_t)Wire.read() << 8);
  value |= ((uint32_t)Wire.read());

  return value;
}


// ============================================================================
// READ A 40-BIT INA228 REGISTER
// ============================================================================

uint64_t readRegister40(uint8_t reg) {

  Wire.beginTransmission(INA228_ADDRESS);

  Wire.write(reg);

  if (Wire.endTransmission(false) != 0) {
    Serial.println("ERROR: could not select 40-bit register");
    return 0;
  }

  if (Wire.requestFrom(INA228_ADDRESS, (uint8_t)5) != 5) {
    Serial.println("ERROR: expected 5 bytes");
    return 0;
  }

  uint64_t value = 0;

  // Build the 40-bit value one byte at a time.
  for (int i = 0; i < 5; i++) {

    value = (value << 8) | Wire.read();
  }

  return value;
}


// ============================================================================
// SIGN-EXTEND A 20-BIT VALUE
// ============================================================================

int32_t signExtend20(uint32_t value) {

  value &= 0xFFFFF;

  if (value & 0x80000) {
    value |= 0xFFF00000;
  }

  return (int32_t)value;
}


// ============================================================================
// SIGN-EXTEND A 40-BIT VALUE
// ============================================================================

int64_t signExtend40(uint64_t value) {

  value &= 0xFFFFFFFFFFULL;

  if (value & 0x8000000000ULL) {
    value |= 0xFFFFFF0000000000ULL;
  }

  return (int64_t)value;
}


// ============================================================================
// RESET INA228 ENERGY AND CHARGE ACCUMULATORS
// ============================================================================

void resetAccumulators() {

  uint16_t config = readRegister16(REG_CONFIG);

  // Set bit 14, RSTACC.
  config |= (1 << 14);

  writeRegister16(REG_CONFIG, config);
}


// ============================================================================
// CONFIGURE INA228
// ============================================================================
//
// We put this in its own function because setup() has two possible paths:
//
//     first boot
//     timer wake
//
// Both need to make sure I2C is working and the INA configuration is valid.

void configureINA228() {

  // Continuous bus + shunt + temperature conversion
  // with 64-sample averaging.
  writeRegister16(REG_ADC_CONFIG, 0xFB6B);

  // Set current calibration.
  writeRegister16(REG_SHUNT_CAL, SHUNT_CAL_VALUE);
}


// ============================================================================
// READ AND PRINT CURRENT INA228 STATE
// ============================================================================

void printMeasurements() {

  // --------------------------------------------------------------------------
  // BUS VOLTAGE
  // --------------------------------------------------------------------------

  uint32_t rawBusRegister =
      readRegister24(REG_VBUS);

  uint32_t rawBus =
      rawBusRegister >> 4;

  double voltage =
      rawBus * 195.3125e-6;


  // --------------------------------------------------------------------------
  // CURRENT
  // --------------------------------------------------------------------------

  uint32_t rawCurrentRegister =
      readRegister24(REG_CURRENT);

  uint32_t rawCurrentUnsigned =
      rawCurrentRegister >> 4;

  int32_t rawCurrent =
      signExtend20(rawCurrentUnsigned);

  double current =
      rawCurrent * CURRENT_LSB;


  // --------------------------------------------------------------------------
  // POWER
  // --------------------------------------------------------------------------

  uint32_t rawPower =
      readRegister24(REG_POWER);

  double power =
      rawPower * 3.2 * CURRENT_LSB;


  // --------------------------------------------------------------------------
  // CHARGE
  // --------------------------------------------------------------------------

  uint64_t rawChargeUnsigned =
      readRegister40(REG_CHARGE);

  int64_t rawCharge =
      signExtend40(rawChargeUnsigned);

  double chargeCoulombs =
      rawCharge * CURRENT_LSB;

  double ampHours =
      chargeCoulombs / 3600.0;


  // --------------------------------------------------------------------------
  // ENERGY
  // --------------------------------------------------------------------------

  uint64_t rawEnergy =
      readRegister40(REG_ENERGY);

  double energyJoules =
      rawEnergy * 16.0 * 3.2 * CURRENT_LSB;

  double wattHours =
      energyJoules / 3600.0;


  // --------------------------------------------------------------------------
  // PRINT
  // --------------------------------------------------------------------------

  Serial.print("Voltage: ");
  Serial.print(voltage, 4);
  Serial.println(" V");

  Serial.print("Current: ");
  Serial.print(current * 1000.0, 3);
  Serial.println(" mA");

  Serial.print("Power:   ");
  Serial.print(power * 1000.0, 3);
  Serial.println(" mW");

  Serial.print("Charge accumulated: ");
  Serial.print(ampHours * 1000.0, 4);
  Serial.println(" mAh");

  Serial.print("Energy accumulated: ");
  Serial.print(wattHours * 1000.0, 4);
  Serial.println(" mWh");
}


// ============================================================================
// SETUP
// ============================================================================
//
// Remember:
//
// Deep sleep wake causes a fresh boot.
//
// Therefore setup() runs again after the 60-second sleep.

void setup() {

  Serial.begin(115200);

  // Give USB Serial time to appear on the Mac.
  delay(2000);

  // Start I2C.
  //
  // XIAO ESP32-C3:
  //     D4 = SDA
  //     D5 = SCL
  Wire.begin(D4, D5);


  Serial.println();
  Serial.println("INA228 deep-sleep accumulation test");
  Serial.println("===================================");


  // ==========================================================================
  // FIND OUT WHY THIS BOOT HAPPENED
  // ==========================================================================

  esp_sleep_wakeup_cause_t wakeupCause =
      esp_sleep_get_wakeup_cause();


  // ==========================================================================
  // CASE 1:
  // WE WOKE FROM THE TIMER
  // ==========================================================================

  if (wakeupCause == ESP_SLEEP_WAKEUP_TIMER) {

    Serial.println("Wake reason: TIMER");
    Serial.println();

    // IMPORTANT:
    //
    // Do NOT reset the INA accumulators here.
    //
    // We want to read everything the INA accumulated while the ESP32 slept.


    // Make sure communication/calibration are valid.
    //
    // Writing ADC_CONFIG and SHUNT_CAL does NOT clear ENERGY or CHARGE.
    configureINA228();


    Serial.println("Measurements after 60-second sleep:");
    Serial.println();

    printMeasurements();

    Serial.println();
    Serial.println("Test complete.");
    Serial.println();
    Serial.println("Expected with roughly 16 mA / 25.7 mW load:");
    Serial.println("Charge: about 0.267 mAh");
    Serial.println("Energy: about 0.428 mWh");

    // Stop here.
    //
    // We deliberately do NOT sleep again.
    //
    // This keeps the USB connection alive so you can read the result.
    while (true) {
      delay(1000);
    }
  }


  // ==========================================================================
  // CASE 2:
  // NORMAL POWER-ON OR RESET
  // ==========================================================================

  Serial.println("Wake reason: normal power-on/reset");
  Serial.println();


  // Configure the INA.
  configureINA228();


  // ==========================================================================
  // LET THE INA SETTLE
  // ==========================================================================

  // Because we're using 64-sample averaging, the first measurement immediately
  // after startup may not represent the real stable current yet.
  //
  // We already saw this earlier:
  //
  //     first reading ~3 mA
  //     later readings ~16 mA
  //
  // So give the INA some time to settle BEFORE resetting the accumulators.
  Serial.println("Waiting 2 seconds for INA228 measurements to settle...");

  delay(2000);


  // Show the current operating state before starting the test.
  Serial.println();
  Serial.println("Current measurements before sleep:");

  printMeasurements();


  // ==========================================================================
  // RESET HARDWARE ACCUMULATORS
  // ==========================================================================

  Serial.println();
  Serial.println("Resetting ENERGY and CHARGE to zero...");

  resetAccumulators();


  // ==========================================================================
  // CONFIGURE THE ESP32 WAKE TIMER
  // ==========================================================================

  // ESP32 sleep timers use microseconds.
  //
  // 1 second = 1,000,000 microseconds.
  //
  // Therefore:
  //
  // 60 seconds × 1,000,000
  // = 60,000,000 microseconds.
  uint64_t sleepMicroseconds =
      SLEEP_SECONDS * 1000000ULL;


  // Tell the low-power timer:
  //
  //     wake the ESP32 after 60 seconds.
  esp_sleep_enable_timer_wakeup(sleepMicroseconds);


  Serial.println();
  Serial.print("Going to deep sleep for ");
  Serial.print(SLEEP_SECONDS);
  Serial.println(" seconds.");

  Serial.println();
  Serial.println("During sleep:");
  Serial.println("  ESP32 CPU should be off.");
  Serial.println("  INA228 should remain powered.");
  Serial.println("  INA228 should continue accumulating.");

  Serial.flush();


  // ==========================================================================
  // DEEP SLEEP
  // ==========================================================================

  esp_deep_sleep_start();


  // Nothing below this line will execute.
}


// ============================================================================
// LOOP
// ============================================================================
//
// We don't need loop() for this experiment.
//
// setup() handles:
//
//     initial boot
//     sleep
//     timer wake
//     result display

void loop() {
}