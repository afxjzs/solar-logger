#include <Wire.h>

// ============================================================================
// INA228 REGISTER ADDRESSES
// ============================================================================

// I2C address we already confirmed with the scanner.
constexpr uint8_t INA228_ADDRESS = 0x40;

// Main configuration register.
constexpr uint8_t REG_CONFIG = 0x00;

// ADC configuration register.
constexpr uint8_t REG_ADC_CONFIG = 0x01;

// Shunt calibration register.
//
// This is the register that teaches the INA228 how to convert measured
// shunt voltage into current.
constexpr uint8_t REG_SHUNT_CAL = 0x02;

// Bus voltage register.
constexpr uint8_t REG_VBUS = 0x05;

// Calculated current register.
constexpr uint8_t REG_CURRENT = 0x07;

// Calculated power register.
constexpr uint8_t REG_POWER = 0x08;

// Hardware accumulated energy register.
constexpr uint8_t REG_ENERGY = 0x09;

// Hardware accumulated charge register.
constexpr uint8_t REG_CHARGE = 0x0A;

// ============================================================================
// SOFTWARE ACCUMULATOR VARIABLES
// ============================================================================

// These totals belong to the ESP32.
//
// They are completely separate from the INA228's internal ENERGY and CHARGE
// registers.
//
// This lets us compare our own math against the INA228 hardware.
double softwareAmpHours = 0.0;
double softwareWattHours = 0.0;


// millis() gives us the ESP32's uptime in milliseconds.
//
// We'll remember the previous time so we can calculate how long each
// measurement interval lasted.
uint32_t previousMillis = 0;


// ============================================================================
// OUR CALIBRATION VALUES
// ============================================================================

// This is the effective shunt resistance we measured on this specific board.
//
// The resistor is marked R015, but our bench calibration showed that the
// effective resistance is closer to 15.62 milliohms.
constexpr double SHUNT_OHMS = 0.01562;


// CURRENT_LSB tells the INA228 how much current one CURRENT-register count
// represents.
//
// We are choosing:
//
//     4 microamps per count
//
// 4 microamps = 0.000004 amps.
//
// Why 4 uA?
//
// The CURRENT register is signed 20-bit, so its positive range is about
// 524,287 counts.
//
//     524287 × 0.000004 A
//     ≈ 2.097 A
//
// That gives us a little over 2 amps of measurement range, which is plenty
// for the solar charger while still giving us very fine resolution.
constexpr double CURRENT_LSB = 0.000004;


// The INA228 datasheet gives us:
//
// SHUNT_CAL = 13107.2 × 10^6 × CURRENT_LSB × RSHUNT
//
// With:
//
// CURRENT_LSB = 0.000004 A
// RSHUNT      = 0.01562 ohms
//
// we get approximately:
//
// SHUNT_CAL = 819
//
// ADCRANGE is 0, so we do NOT multiply this by 4.
constexpr uint16_t SHUNT_CAL_VALUE = 819;


// ============================================================================
// READ A 16-BIT REGISTER
// ============================================================================

uint16_t readRegister16(uint8_t reg) {

  // Start an I2C transaction with the INA228.
  Wire.beginTransmission(INA228_ADDRESS);

  // Tell the INA228 which register we want.
  Wire.write(reg);

  // Keep the bus active because a read immediately follows.
  if (Wire.endTransmission(false) != 0) {
    Serial.println("ERROR: could not select 16-bit register");
    return 0;
  }

  // Ask for two bytes.
  if (Wire.requestFrom(INA228_ADDRESS, (uint8_t)2) != 2) {
    Serial.println("ERROR: expected 2 bytes");
    return 0;
  }

  // INA228 sends the high byte first.
  uint16_t value = ((uint16_t)Wire.read() << 8);

  // Add the low byte.
  value |= (uint16_t)Wire.read();

  return value;
}


// ============================================================================
// WRITE A 16-BIT REGISTER
// ============================================================================

void writeRegister16(uint8_t reg, uint16_t value) {

  // Start talking to the INA228.
  Wire.beginTransmission(INA228_ADDRESS);

  // Select the register.
  Wire.write(reg);

  // Send the upper 8 bits.
  Wire.write((uint8_t)(value >> 8));

  // Send the lower 8 bits.
  Wire.write((uint8_t)(value & 0xFF));

  // Finish the transaction.
  Wire.endTransmission();
}


// ============================================================================
// READ A 24-BIT REGISTER
// ============================================================================

uint32_t readRegister24(uint8_t reg) {

  // Select the register we want to read.
  Wire.beginTransmission(INA228_ADDRESS);
  Wire.write(reg);

  if (Wire.endTransmission(false) != 0) {
    Serial.println("ERROR: could not select 24-bit register");
    return 0;
  }

  // Ask the INA228 for three bytes.
  if (Wire.requestFrom(INA228_ADDRESS, (uint8_t)3) != 3) {
    Serial.println("ERROR: expected 3 bytes");
    return 0;
  }

  // Assemble the three bytes into one integer.
  uint32_t value = 0;

  value |= ((uint32_t)Wire.read() << 16);
  value |= ((uint32_t)Wire.read() << 8);
  value |= ((uint32_t)Wire.read());

  return value;
}


// ============================================================================
// READ A 40-BIT REGISTER
// ============================================================================

uint64_t readRegister40(uint8_t reg) {

  // ENERGY and CHARGE are five bytes wide:
  //
  //     5 bytes × 8 bits = 40 bits
  //
  // Arduino does not have a native 40-bit integer type, so we store the value
  // in a 64-bit integer.
  Wire.beginTransmission(INA228_ADDRESS);
  Wire.write(reg);

  if (Wire.endTransmission(false) != 0) {
    Serial.println("ERROR: could not select 40-bit register");
    return 0;
  }

  // Ask for five bytes.
  if (Wire.requestFrom(INA228_ADDRESS, (uint8_t)5) != 5) {
    Serial.println("ERROR: expected 5 bytes");
    return 0;
  }

  uint64_t value = 0;

  // Shift the existing value left by 8 bits before adding each new byte.
  //
  // This rebuilds the five-byte number in the correct order.
  for (int i = 0; i < 5; i++) {
    value = (value << 8) | Wire.read();
  }

  return value;
}


// ============================================================================
// SIGN-EXTEND A 20-BIT CURRENT VALUE
// ============================================================================

int32_t signExtend20(uint32_t value) {

  // Keep only the 20 measurement bits.
  value &= 0xFFFFF;

  // Bit 19 is the sign bit.
  if (value & 0x80000) {

    // Fill the upper bits with 1s so the ESP32 interprets it as negative.
    value |= 0xFFF00000;
  }

  return (int32_t)value;
}


// ============================================================================
// SIGN-EXTEND A 40-BIT CHARGE VALUE
// ============================================================================

int64_t signExtend40(uint64_t value) {

  // Only the lower 40 bits belong to the INA228 measurement.
  value &= 0xFFFFFFFFFFULL;

  // Bit 39 is the sign bit.
  if (value & 0x8000000000ULL) {

    // If the value is negative, fill bits 63:40 with 1s.
    value |= 0xFFFFFF0000000000ULL;
  }

  return (int64_t)value;
}


// ============================================================================
// RESET THE INA228 ENERGY AND CHARGE ACCUMULATORS
// ============================================================================

void resetAccumulators() {

  // Read the current CONFIG register first.
  uint16_t config = readRegister16(REG_CONFIG);

  // CONFIG bit 14 is RSTACC.
  //
  // Writing a 1 here clears both:
  //
  //     ENERGY
  //     CHARGE
  //
  // The INA228 then continues accumulating again from zero.
  config |= (1 << 14);

  // Write the modified CONFIG value back.
  writeRegister16(REG_CONFIG, config);
}


// ============================================================================
// SETUP
// ============================================================================

void setup() {

  // Start Serial Monitor output.
  Serial.begin(115200);

  // Give USB serial a moment to initialize.
  delay(2000);

  // Start I2C on the XIAO ESP32-C3.
  //
  // D4 = SDA
  // D5 = SCL
  Wire.begin(D4, D5);

  Serial.println();
  Serial.println("INA228 hardware accumulator test");
  Serial.println("--------------------------------");


  // --------------------------------------------------------------------------
  // CONFIGURE ADC
  // --------------------------------------------------------------------------

  // Use continuous measurement with 64-sample averaging.
  //
  // This is the same ADC configuration that worked well in our previous test.
  writeRegister16(REG_ADC_CONFIG, 0xFB6B);


  // --------------------------------------------------------------------------
  // CONFIGURE INA228 CURRENT CALCULATION
  // --------------------------------------------------------------------------

  // Write our calculated SHUNT_CAL value.
  //
  // After this, the INA228 itself can calculate CURRENT, POWER, ENERGY,
  // and CHARGE from the measured shunt voltage.
  writeRegister16(REG_SHUNT_CAL, SHUNT_CAL_VALUE);


  // --------------------------------------------------------------------------
  // RESET HARDWARE ACCUMULATORS
  // --------------------------------------------------------------------------

  // Start this test with ENERGY and CHARGE equal to zero.
  resetAccumulators();


  // Give the INA228 a short moment to start taking measurements.
  delay(100);


  // --------------------------------------------------------------------------
  // VERIFY CONFIGURATION
  // --------------------------------------------------------------------------

  uint16_t config = readRegister16(REG_CONFIG);
  uint16_t adcConfig = readRegister16(REG_ADC_CONFIG);
  uint16_t shuntCal = readRegister16(REG_SHUNT_CAL);

  Serial.print("CONFIG:     0x");
  Serial.println(config, HEX);

  Serial.print("ADC_CONFIG: 0x");
  Serial.println(adcConfig, HEX);

  Serial.print("SHUNT_CAL:  ");
  Serial.println(shuntCal);

  Serial.print("CURRENT_LSB: ");
  Serial.print(CURRENT_LSB * 1000000.0, 3);
  Serial.println(" uA/count");

  Serial.println();
  // Record our software integration starting time.
  //
  // From this point forward, the ESP32 will independently integrate current
  // and power while the INA228 simultaneously does the same job internally.
  previousMillis = millis();
}


// ============================================================================
// MAIN LOOP
// ============================================================================

void loop() {

  // ==========================================================================
  // 1. FIGURE OUT HOW MUCH TIME HAS PASSED
  // ==========================================================================

  // Ask the ESP32 how many milliseconds it has been running.
  uint32_t currentMillis = millis();

  // Subtract the previous timestamp from the current timestamp.
  //
  // Example:
  //
  // currentMillis  = 12000
  // previousMillis = 10000
  //
  // elapsedMillis  = 2000 ms
  uint32_t elapsedMillis = currentMillis - previousMillis;

  // Save this timestamp for the next loop.
  previousMillis = currentMillis;

  // Convert milliseconds into hours.
  //
  // 1 hour = 3,600,000 milliseconds.
  double elapsedHours = elapsedMillis / 3600000.0;


  // ==========================================================================
  // 2. READ BUS VOLTAGE FROM THE INA228
  // ==========================================================================

  // Ask the INA228 for its raw VBUS register.
  uint32_t rawBusRegister = readRegister24(REG_VBUS);

  // The useful 20-bit measurement occupies bits 23:4.
  //
  // Shift four places right to discard the unused lower bits.
  uint32_t rawBus = rawBusRegister >> 4;

  // Convert ADC counts into actual volts.
  double busVoltage = rawBus * 195.3125e-6;


  // ==========================================================================
  // 3. READ CURRENT CALCULATED BY THE INA228
  // ==========================================================================

  // Ask the INA228 for its CURRENT register.
  uint32_t rawCurrentRegister = readRegister24(REG_CURRENT);

  // CURRENT is a signed 20-bit measurement stored in bits 23:4.
  uint32_t rawCurrentUnsigned = rawCurrentRegister >> 4;

  // Convert the 20-bit signed value into something the ESP32 understands.
  int32_t rawCurrent = signExtend20(rawCurrentUnsigned);

  // Each current count represents CURRENT_LSB amps.
  //
  // We configured CURRENT_LSB as:
  //
  //     0.000004 A
  //
  // which is:
  //
  //     4 microamps
  double current = rawCurrent * CURRENT_LSB;


  // ==========================================================================
  // 4. READ POWER CALCULATED BY THE INA228
  // ==========================================================================

  // POWER is a 24-bit unsigned register.
  uint32_t rawPower = readRegister24(REG_POWER);

  // Convert the INA228's raw power number into watts.
  double power = rawPower * 3.2 * CURRENT_LSB;


  // ==========================================================================
  // 5. SOFTWARE INTEGRATION
  // ==========================================================================

  // THIS is the method from our previous sketch.
  //
  // The ESP32 takes the instantaneous current and multiplies it by the amount
  // of time since the previous sample.
  //
  // amps × hours = amp-hours
  softwareAmpHours += current * elapsedHours;

  // Same idea for energy:
  //
  // watts × hours = watt-hours
  softwareWattHours += power * elapsedHours;


  // ==========================================================================
  // 6. READ THE INA228 HARDWARE CHARGE ACCUMULATOR
  // ==========================================================================

  // Ask the INA228 for its internal 40-bit CHARGE register.
  uint64_t rawChargeUnsigned = readRegister40(REG_CHARGE);

  // Charge can be positive or negative because current can flow either way.
  int64_t rawCharge = signExtend40(rawChargeUnsigned);

  // Convert the hardware accumulator into coulombs.
  double chargeCoulombs = rawCharge * CURRENT_LSB;

  // 3600 coulombs = 1 amp-hour.
  double hardwareAmpHours = chargeCoulombs / 3600.0;


  // ==========================================================================
  // 7. READ THE INA228 HARDWARE ENERGY ACCUMULATOR
  // ==========================================================================

  // Ask the INA228 for its internal ENERGY register.
  uint64_t rawEnergy = readRegister40(REG_ENERGY);

  // Convert the raw accumulator into joules.
  double energyJoules =
      rawEnergy * 16.0 * 3.2 * CURRENT_LSB;

  // 3600 joules = 1 watt-hour.
  double hardwareWattHours =
      energyJoules / 3600.0;


  // ==========================================================================
  // 8. CALCULATE DIFFERENCE BETWEEN THE TWO METHODS
  // ==========================================================================

  // Subtract our ESP32 software result from the INA228 hardware result.
  //
  // Ideally these stay very close to zero.
  double chargeDifference =
      hardwareAmpHours - softwareAmpHours;

  double energyDifference =
      hardwareWattHours - softwareWattHours;


  // ==========================================================================
  // 9. PRINT THE INSTANTANEOUS MEASUREMENTS
  // ==========================================================================

  Serial.println("--------------------------------------------------");

  Serial.print("Voltage: ");
  Serial.print(busVoltage, 4);
  Serial.print(" V");

  Serial.print("    Current: ");
  Serial.print(current * 1000.0, 3);
  Serial.print(" mA");

  Serial.print("    Power: ");
  Serial.print(power * 1000.0, 3);
  Serial.println(" mW");


  // ==========================================================================
  // 10. PRINT CHARGE COMPARISON
  // ==========================================================================

  Serial.print("Charge   HW: ");
  Serial.print(hardwareAmpHours * 1000.0, 4);
  Serial.print(" mAh");

  Serial.print("    SW: ");
  Serial.print(softwareAmpHours * 1000.0, 4);
  Serial.print(" mAh");

  Serial.print("    Difference: ");
  Serial.print(chargeDifference * 1000.0, 4);
  Serial.println(" mAh");


  // ==========================================================================
  // 11. PRINT ENERGY COMPARISON
  // ==========================================================================

  Serial.print("Energy   HW: ");
  Serial.print(hardwareWattHours * 1000.0, 4);
  Serial.print(" mWh");

  Serial.print("    SW: ");
  Serial.print(softwareWattHours * 1000.0, 4);
  Serial.print(" mWh");

  Serial.print("    Difference: ");
  Serial.print(energyDifference * 1000.0, 4);
  Serial.println(" mWh");

  Serial.println();


  // ==========================================================================
  // 12. WAIT TWO SECONDS
  // ==========================================================================

  // During this delay:
  //
  // ESP32 software integration:
  //     NOT doing anything.
  //
  // INA228 hardware accumulator:
  //     STILL continuously measuring and accumulating.
  //
  // When loop() runs again, the ESP32 accounts for those two seconds using
  // elapsedMillis, while the INA228 has actually been accumulating internally
  // throughout the interval.
  delay(2000);
}