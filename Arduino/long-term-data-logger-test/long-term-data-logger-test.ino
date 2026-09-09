#include <Wire.h>
#include "esp_sleep.h"

// ============================================================================
// INA228 REGISTER ADDRESSES
// ============================================================================

// I2C address of our INA228.
// We already confirmed this earlier with the I2C scanner.
constexpr uint8_t INA228_ADDRESS = 0x40;

// General configuration register.
constexpr uint8_t REG_CONFIG = 0x00;

// ADC configuration register.
constexpr uint8_t REG_ADC_CONFIG = 0x01;

// Shunt calibration register.
constexpr uint8_t REG_SHUNT_CAL = 0x02;

// Bus voltage register.
constexpr uint8_t REG_VBUS = 0x05;

// Internal die temperature register.
constexpr uint8_t REG_DIETEMP = 0x06;

// Calculated current register.
constexpr uint8_t REG_CURRENT = 0x07;

// Calculated power register.
constexpr uint8_t REG_POWER = 0x08;

// Hardware energy accumulator.
constexpr uint8_t REG_ENERGY = 0x09;

// Hardware charge accumulator.
constexpr uint8_t REG_CHARGE = 0x0A;


// ============================================================================
// CALIBRATION
// ============================================================================

// Our breakout's effective shunt resistance.
//
// The resistor is marked R015, nominally 15 milliohms,
// but our DMM-based calibration showed about 15.62 milliohms.
constexpr double SHUNT_OHMS = 0.01562;

// Each CURRENT register count represents 4 microamps.
//
// 4 microamps = 0.000004 amps.
constexpr double CURRENT_LSB = 0.000004;

// Calibration value calculated from:
//
// SHUNT_CAL = 13107.2 × 10^6 × CURRENT_LSB × RSHUNT
//
// With our values, this is approximately 819.
constexpr uint16_t SHUNT_CAL_VALUE = 819;


// ============================================================================
// LOGGER SETTINGS
// ============================================================================

// Sleep for 60 seconds between samples.
//
// This is intentionally short for the bench test.
//
// In the car, we may eventually use something more like:
//     5 minutes
//     15 minutes
//     30 minutes
//
// But one minute gives us lots of cycles while testing.
constexpr uint64_t SLEEP_SECONDS = 60;


// ============================================================================
// RTC PERSISTENT VARIABLES
// ============================================================================
//
// This is a NEW concept.
//
// Normal ESP32 RAM is mostly lost during deep sleep.
//
// But the ESP32 has a small low-power memory area called RTC memory.
//
// Variables marked:
//
//     RTC_DATA_ATTR
//
// are stored there.
//
// That means these values survive deep sleep.
//
// They do NOT necessarily survive:
//     unplugging USB
//     complete power loss
//     reflashing firmware
//
// For this test, that is exactly what we want.


// Counts how many completed sleep intervals we have recorded.
RTC_DATA_ATTR uint32_t intervalNumber = 0;


// Running total of charge across all intervals.
//
// We store milliamp-hours because that is convenient for display.
RTC_DATA_ATTR double totalCharge_mAh = 0.0;


// Running total of energy across all intervals.
//
// We store milliwatt-hours.
RTC_DATA_ATTR double totalEnergy_mWh = 0.0;


// ============================================================================
// READ A 16-BIT REGISTER
// ============================================================================
//
// The INA228 sends register values over I2C as individual bytes.
//
// A 16-bit value is two bytes:
//
//     high byte
//     low byte

uint16_t readRegister16(uint8_t reg) {

  // Begin an I2C transaction with device 0x40.
  Wire.beginTransmission(INA228_ADDRESS);

  // Tell the INA228 which register we want to read.
  Wire.write(reg);

  // false means:
  //
  //     "Don't completely release the I2C bus yet.
  //      I am about to request data."
  if (Wire.endTransmission(false) != 0) {

    Serial.println("ERROR: could not select 16-bit register");

    return 0;
  }

  // Request exactly 2 bytes.
  if (Wire.requestFrom(INA228_ADDRESS, (uint8_t)2) != 2) {

    Serial.println("ERROR: expected 2 bytes");

    return 0;
  }

  // First byte becomes bits 15 through 8.
  uint16_t value =
      ((uint16_t)Wire.read() << 8);

  // Second byte becomes bits 7 through 0.
  value |=
      (uint16_t)Wire.read();

  return value;
}


// ============================================================================
// WRITE A 16-BIT REGISTER
// ============================================================================

void writeRegister16(uint8_t reg, uint16_t value) {

  Wire.beginTransmission(INA228_ADDRESS);

  // Select the register.
  Wire.write(reg);

  // Send the upper 8 bits.
  Wire.write(
      (uint8_t)(value >> 8)
  );

  // Send the lower 8 bits.
  Wire.write(
      (uint8_t)(value & 0xFF)
  );

  Wire.endTransmission();
}


// ============================================================================
// READ A 24-BIT REGISTER
// ============================================================================
//
// C++ does not normally have a uint24_t type.
//
// So we read three bytes and store them inside a uint32_t.

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

  // Byte 1 goes into bits 23:16.
  value |=
      ((uint32_t)Wire.read() << 16);

  // Byte 2 goes into bits 15:8.
  value |=
      ((uint32_t)Wire.read() << 8);

  // Byte 3 goes into bits 7:0.
  value |=
      ((uint32_t)Wire.read());

  return value;
}


// ============================================================================
// READ A 40-BIT REGISTER
// ============================================================================
//
// ENERGY and CHARGE are 40-bit registers.
//
// 40 bits = 5 bytes.
//
// We store them inside a uint64_t because 64 bits is large enough.

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

  // Read five bytes.
  //
  // Each iteration:
  //
  //     shift previous value left by 8 bits
  //     insert the new byte at the bottom
  //
  // After five iterations, we have reconstructed the full 40-bit number.
  for (int i = 0; i < 5; i++) {

    value =
        (value << 8) | Wire.read();
  }

  return value;
}


// ============================================================================
// SIGN-EXTEND A 20-BIT NUMBER
// ============================================================================
//
// CURRENT is signed.
//
// That means:
//
//     positive current
//     zero current
//     negative current
//
// The INA stores it as signed 20-bit two's-complement.
//
// ESP32 normally works with signed 32-bit integers,
// so we convert it into an int32_t.

int32_t signExtend20(uint32_t value) {

  // Keep only the bottom 20 bits.
  value &= 0xFFFFF;

  // Bit 19 is the sign bit.
  //
  // If it is 1, the value is negative.
  if (value & 0x80000) {

    // Fill the unused upper bits with 1s.
    value |= 0xFFF00000;
  }

  return (int32_t)value;
}


// ============================================================================
// SIGN-EXTEND A 40-BIT NUMBER
// ============================================================================
//
// CHARGE is also signed.
//
// This allows the INA228 to represent charge flowing in either direction.

int64_t signExtend40(uint64_t value) {

  // Keep only 40 bits.
  value &= 0xFFFFFFFFFFULL;

  // Bit 39 is the sign bit.
  if (value & 0x8000000000ULL) {

    // Fill unused upper bits with 1s.
    value |= 0xFFFFFF0000000000ULL;
  }

  return (int64_t)value;
}


// ============================================================================
// CONFIGURE INA228
// ============================================================================

void configureINA228() {

  // Continuous bus + shunt + temperature conversion.
  //
  // 64-sample averaging.
  //
  // This is the same stable configuration we have already tested.
  writeRegister16(
      REG_ADC_CONFIG,
      0xFB6B
  );

  // Apply our current calibration.
  writeRegister16(
      REG_SHUNT_CAL,
      SHUNT_CAL_VALUE
  );
}


// ============================================================================
// RESET INA228 ENERGY AND CHARGE
// ============================================================================

void resetAccumulators() {

  // Read the existing CONFIG register first.
  uint16_t config =
      readRegister16(REG_CONFIG);

  // Bit 14 is RSTACC.
  //
  // Setting it to 1 clears:
  //
  //     ENERGY
  //     CHARGE
  config |=
      (1 << 14);

  writeRegister16(
      REG_CONFIG,
      config
  );
}


// ============================================================================
// READ BUS VOLTAGE
// ============================================================================

double readBusVoltage() {

  uint32_t rawRegister =
      readRegister24(REG_VBUS);

  // VBUS uses bits 23:4.
  uint32_t rawValue =
      rawRegister >> 4;

  // Each count represents 195.3125 microvolts.
  return
      rawValue * 195.3125e-6;
}


// ============================================================================
// READ CURRENT
// ============================================================================

double readCurrent() {

  uint32_t rawRegister =
      readRegister24(REG_CURRENT);

  // CURRENT also uses bits 23:4.
  uint32_t rawUnsigned =
      rawRegister >> 4;

  int32_t rawSigned =
      signExtend20(rawUnsigned);

  // Convert register counts into amps.
  return
      rawSigned * CURRENT_LSB;
}


// ============================================================================
// READ POWER
// ============================================================================

double readPower() {

  uint32_t rawPower =
      readRegister24(REG_POWER);

  // INA228 power scaling:
  //
  // Power [W] =
  //
  //     raw POWER
  //     × 3.2
  //     × CURRENT_LSB
  return
      rawPower * 3.2 * CURRENT_LSB;
}


// ============================================================================
// READ TEMPERATURE
// ============================================================================

double readTemperatureC() {

  // DIETEMP is a signed 16-bit value.
  int16_t rawTemperature =
      (int16_t)readRegister16(REG_DIETEMP);

  // Each count represents 0.0078125 degrees Celsius.
  return
      rawTemperature * 0.0078125;
}


// ============================================================================
// READ ACCUMULATED CHARGE
// ============================================================================

double readCharge_mAh() {

  uint64_t rawUnsigned =
      readRegister40(REG_CHARGE);

  int64_t rawSigned =
      signExtend40(rawUnsigned);

  // CURRENT_LSB scaling gives accumulated charge in coulombs.
  double coulombs =
      rawSigned * CURRENT_LSB;

  // 3600 coulombs = 1 amp-hour.
  double ampHours =
      coulombs / 3600.0;

  // Convert Ah into mAh.
  return
      ampHours * 1000.0;
}


// ============================================================================
// READ ACCUMULATED ENERGY
// ============================================================================

double readEnergy_mWh() {

  uint64_t rawEnergy =
      readRegister40(REG_ENERGY);

  // INA228 energy scaling produces joules.
  double joules =
      rawEnergy
      * 16.0
      * 3.2
      * CURRENT_LSB;

  // 3600 joules = 1 watt-hour.
  double wattHours =
      joules / 3600.0;

  // Convert Wh into mWh.
  return
      wattHours * 1000.0;
}


// ============================================================================
// GO TO DEEP SLEEP
// ============================================================================

void goToSleep() {

  // ESP32 sleep timer uses microseconds.
  //
  // 1 second = 1,000,000 microseconds.
  uint64_t sleepMicroseconds =
      SLEEP_SECONDS * 1000000ULL;

  // Configure the low-power timer.
  esp_sleep_enable_timer_wakeup(
      sleepMicroseconds
  );

  Serial.println();
  Serial.print("Sleeping for ");
  Serial.print(SLEEP_SECONDS);
  Serial.println(" seconds...");

  Serial.println();

  // Make sure all serial output has physically left the ESP32
  // before USB disappears.
  Serial.flush();

  // Shut down the main CPU.
  //
  // INA228 remains powered from the XIAO 3.3 V rail,
  // which our previous test proved stays alive during deep sleep.
  esp_deep_sleep_start();
}


// ============================================================================
// SETUP
// ============================================================================
//
// Deep-sleep wake causes the ESP32 to boot again.
//
// So every interval starts here.

void setup() {

  // Start USB Serial.
  Serial.begin(115200);

  // Give macOS / USB some time to establish the serial connection.
  delay(2000);

  // Start I2C.
  //
  // XIAO ESP32-C3:
  //
  //     D4 = SDA
  //     D5 = SCL
  Wire.begin(D4, D5);


  Serial.println();
  Serial.println("INA228 continuous deep-sleep logger");
  Serial.println("===================================");


  // ==========================================================================
  // DETERMINE WHY WE BOOTED
  // ==========================================================================

  esp_sleep_wakeup_cause_t wakeupCause =
      esp_sleep_get_wakeup_cause();


  // ==========================================================================
  // TIMER WAKE
  // ==========================================================================

  if (wakeupCause == ESP_SLEEP_WAKEUP_TIMER) {

    // We completed another sleep interval.
    intervalNumber++;

    Serial.println("Wake reason: TIMER");
    Serial.println();


    // ------------------------------------------------------------------------
    // IMPORTANT:
    //
    // READ THE ACCUMULATORS BEFORE RESETTING THEM.
    //
    // They contain everything measured during the sleep interval.
    // ------------------------------------------------------------------------


    // Reapply configuration.
    //
    // This does NOT clear ENERGY or CHARGE.
    configureINA228();


    // Read current instantaneous measurements.
    double voltage =
        readBusVoltage();

    double current =
        readCurrent();

    double power =
        readPower();

    double temperatureC =
        readTemperatureC();


    // Read accumulated measurements for JUST the completed interval.
    double intervalCharge_mAh =
        readCharge_mAh();

    double intervalEnergy_mWh =
        readEnergy_mWh();


    // Add this interval to our running totals stored in RTC memory.
    totalCharge_mAh +=
        intervalCharge_mAh;

    totalEnergy_mWh +=
        intervalEnergy_mWh;


    // ------------------------------------------------------------------------
    // CALCULATE AVERAGE CURRENT AND POWER FOR THE INTERVAL
    // ------------------------------------------------------------------------
    //
    // This is one of the useful things accumulated charge/energy lets us do.
    //
    // For example:
    //
    //     0.267 mAh collected over 1 minute
    //
    // means average current was:
    //
    //     0.267 mAh / (1/60 hour)
    //
    // = 16.02 mA

    double intervalHours =
        SLEEP_SECONDS / 3600.0;


    double averageCurrent_mA =
        intervalCharge_mAh / intervalHours;


    double averagePower_mW =
        intervalEnergy_mWh / intervalHours;


    // ------------------------------------------------------------------------
    // PRINT THIS INTERVAL
    // ------------------------------------------------------------------------

    Serial.println("------------------------------------------------------------");

    Serial.print("Interval #");
    Serial.println(intervalNumber);

    Serial.print("Interval length: ");
    Serial.print(SLEEP_SECONDS);
    Serial.println(" seconds");

    Serial.println();


    Serial.print("Ending voltage:     ");
    Serial.print(voltage, 4);
    Serial.println(" V");


    Serial.print("Current right now:  ");
    Serial.print(current * 1000.0, 3);
    Serial.println(" mA");


    Serial.print("Power right now:    ");
    Serial.print(power * 1000.0, 3);
    Serial.println(" mW");


    Serial.print("Temperature:        ");
    Serial.print(temperatureC, 2);
    Serial.println(" C");


    Serial.println();


    Serial.print("Interval charge:    ");
    Serial.print(intervalCharge_mAh, 4);
    Serial.println(" mAh");


    Serial.print("Interval energy:    ");
    Serial.print(intervalEnergy_mWh, 4);
    Serial.println(" mWh");


    Serial.print("Average current:    ");
    Serial.print(averageCurrent_mA, 3);
    Serial.println(" mA");


    Serial.print("Average power:      ");
    Serial.print(averagePower_mW, 3);
    Serial.println(" mW");


    Serial.println();


    Serial.print("RUNNING charge:     ");
    Serial.print(totalCharge_mAh, 4);
    Serial.println(" mAh");


    Serial.print("RUNNING energy:     ");
    Serial.print(totalEnergy_mWh, 4);
    Serial.println(" mWh");


    // ------------------------------------------------------------------------
    // START A NEW INTERVAL
    // ------------------------------------------------------------------------

    Serial.println();
    Serial.println("Resetting INA228 accumulators for next interval...");

    resetAccumulators();


    // Sleep again.
    //
    // This causes another fresh boot when the timer expires.
    goToSleep();
  }


  // ==========================================================================
  // NORMAL POWER-ON / RESET
  // ==========================================================================

  Serial.println("Wake reason: normal power-on/reset");
  Serial.println();

  Serial.println("Starting a new continuous logger run.");
  Serial.println();


  // When we intentionally start a new logger session,
  // clear our RTC running totals.
  intervalNumber = 0;

  totalCharge_mAh = 0.0;

  totalEnergy_mWh = 0.0;


  // Configure INA228.
  configureINA228();


  // ==========================================================================
  // LET INA228 SETTLE
  // ==========================================================================

  Serial.println("Waiting 2 seconds for INA228 to settle...");

  delay(2000);


  // Print the initial state.
  double voltage =
      readBusVoltage();

  double current =
      readCurrent();

  double power =
      readPower();

  double temperatureC =
      readTemperatureC();


  Serial.println();

  Serial.println("Initial measurements:");

  Serial.print("Voltage:      ");
  Serial.print(voltage, 4);
  Serial.println(" V");

  Serial.print("Current:      ");
  Serial.print(current * 1000.0, 3);
  Serial.println(" mA");

  Serial.print("Power:        ");
  Serial.print(power * 1000.0, 3);
  Serial.println(" mW");

  Serial.print("Temperature:  ");
  Serial.print(temperatureC, 2);
  Serial.println(" C");


  // ==========================================================================
  // RESET INA ACCUMULATORS
  // ==========================================================================

  Serial.println();

  Serial.println("Resetting INA228 accumulators...");

  resetAccumulators();


  Serial.println();
  Serial.println("Continuous logger started.");
  Serial.println("It will run until you reset/unplug/reflash it.");


  // Start the first sleep interval.
  goToSleep();
}


// ============================================================================
// LOOP
// ============================================================================
//
// We never reach a normal Arduino loop.
//
// Each wake cycle does:
//
//     setup()
//     read data
//     print data
//     sleep
//
// Then the chip reboots.

void loop() {
}