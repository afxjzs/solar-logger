#include <Wire.h>

// ============================================================================
// INA228 REGISTER ADDRESSES
// ============================================================================

// The I2C address of our INA228.
// We confirmed this earlier with the I2C scanner.
constexpr uint8_t INA228_ADDRESS = 0x40;


// Configuration register.
//
// This controls general INA228 behavior, including accumulator reset.
constexpr uint8_t REG_CONFIG = 0x00;


// ADC configuration.
//
// This controls:
//   - which measurements are taken
//   - conversion time
//   - averaging
//   - continuous vs triggered operation
constexpr uint8_t REG_ADC_CONFIG = 0x01;


// Calibration register.
//
// This tells the INA228 how to convert shunt voltage into current.
constexpr uint8_t REG_SHUNT_CAL = 0x02;


// Bus voltage measurement.
constexpr uint8_t REG_VBUS = 0x05;


// Internal die temperature measurement.
constexpr uint8_t REG_DIETEMP = 0x06;


// Current calculated by the INA228.
constexpr uint8_t REG_CURRENT = 0x07;


// Power calculated by the INA228.
constexpr uint8_t REG_POWER = 0x08;


// Hardware accumulated energy.
constexpr uint8_t REG_ENERGY = 0x09;


// Hardware accumulated charge.
constexpr uint8_t REG_CHARGE = 0x0A;


// ============================================================================
// OUR BOARD CALIBRATION
// ============================================================================

// Our shunt is marked R015, nominally 15 milliohms.
//
// Our DMM calibration showed that this particular board behaves more like:
//
//     15.62 milliohms
//
// which is:
//
//     0.01562 ohms
constexpr double SHUNT_OHMS = 0.01562;


// We chose a CURRENT_LSB of 4 microamps per count.
//
// That means:
//
//     INA CURRENT register value 1 = 4 uA
//     INA CURRENT register value 2 = 8 uA
//     ...
//
// This gives us a measurement range of a little over 2 A, which is plenty
// for our solar charger.
constexpr double CURRENT_LSB = 0.000004;


// The INA228 datasheet gives the calibration equation:
//
// SHUNT_CAL = 13107.2 × 10^6 × CURRENT_LSB × RSHUNT
//
// Using:
//
// CURRENT_LSB = 0.000004 A
// RSHUNT      = 0.01562 ohms
//
// gives approximately 819.
constexpr uint16_t SHUNT_CAL_VALUE = 819;


// ============================================================================
// SOFTWARE ACCUMULATORS
// ============================================================================

// We will calculate charge ourselves using:
//
//     current × elapsed time
//
// This lets us compare the ESP32 software result with the INA228 hardware
// CHARGE accumulator.
double softwareAmpHours = 0.0;


// We now have TWO software energy accumulators.
//
// The first uses the INA228 POWER register.
//
//     INA POWER × elapsed time
double softwareWhFromPowerRegister = 0.0;


// The second ignores the POWER register.
//
// Instead, the ESP32 calculates:
//
//     voltage × current
//
// and integrates that over time.
double softwareWhFromVI = 0.0;


// Stores the ESP32 time of the previous sample.
uint32_t previousMillis = 0;


// ============================================================================
// READ A 16-BIT REGISTER
// ============================================================================
//
// Think of this function as:
//
//     "INA228, give me the 16-bit number stored at this address."
//
// A 16-bit number requires two bytes over I2C.

uint16_t readRegister16(uint8_t reg) {

  // Start talking to I2C device 0x40.
  Wire.beginTransmission(INA228_ADDRESS);

  // Tell it which internal register we want.
  Wire.write(reg);

  // false means:
  //
  //     "Do not completely release the I2C bus yet.
  //      I am about to read the answer."
  if (Wire.endTransmission(false) != 0) {
    Serial.println("ERROR: could not select 16-bit register");
    return 0;
  }

  // Request exactly 2 bytes.
  if (Wire.requestFrom(INA228_ADDRESS, (uint8_t)2) != 2) {
    Serial.println("ERROR: expected 2 bytes");
    return 0;
  }

  // First byte is the upper 8 bits.
  uint16_t value = ((uint16_t)Wire.read() << 8);

  // Second byte becomes the lower 8 bits.
  value |= (uint16_t)Wire.read();

  return value;
}


// ============================================================================
// WRITE A 16-BIT REGISTER
// ============================================================================
//
// Think of this as:
//
//     "INA228, put this 16-bit value into this register."

void writeRegister16(uint8_t reg, uint16_t value) {

  Wire.beginTransmission(INA228_ADDRESS);

  // First tell the INA which register we want.
  Wire.write(reg);

  // I2C sends one byte at a time.
  //
  // A uint16_t contains 16 bits, so we split it into two 8-bit pieces.

  // Send the upper 8 bits.
  Wire.write((uint8_t)(value >> 8));

  // Send the lower 8 bits.
  //
  // 0xFF in binary is:
  //
  //     11111111
  //
  // ANDing with it keeps only the lowest 8 bits.
  Wire.write((uint8_t)(value & 0xFF));

  // Send the complete transaction.
  Wire.endTransmission();
}


// ============================================================================
// READ A 24-BIT REGISTER
// ============================================================================
//
// Several INA228 measurements are stored as three bytes:
//
//     3 bytes × 8 bits = 24 bits
//
// C++ does not have a convenient uint24_t type, so we store those 24 bits
// inside a uint32_t.

uint32_t readRegister24(uint8_t reg) {

  Wire.beginTransmission(INA228_ADDRESS);

  // Select the internal register.
  Wire.write(reg);

  if (Wire.endTransmission(false) != 0) {
    Serial.println("ERROR: could not select 24-bit register");
    return 0;
  }

  // Three bytes = 24 bits.
  if (Wire.requestFrom(INA228_ADDRESS, (uint8_t)3) != 3) {
    Serial.println("ERROR: expected 3 bytes");
    return 0;
  }

  uint32_t value = 0;

  // Imagine the INA sends:
  //
  //     AA BB CC
  //
  // We want to construct:
  //
  //     0xAABBCC
  //
  // The first byte therefore moves left 16 bit positions.
  value |= ((uint32_t)Wire.read() << 16);

  // The second moves left 8.
  value |= ((uint32_t)Wire.read() << 8);

  // The final byte already belongs in the bottom 8 bits.
  value |= ((uint32_t)Wire.read());

  return value;
}


// ============================================================================
// READ A 40-BIT REGISTER
// ============================================================================
//
// ENERGY and CHARGE are five bytes:
//
//     5 × 8 = 40 bits
//
// C++ does not normally give us a 40-bit integer type.
//
// So we store the 40-bit number inside a 64-bit integer.

uint64_t readRegister40(uint8_t reg) {

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

  // Read one byte at a time.
  //
  // Each time:
  //
  //     1. Move the number we already have left by 8 bits.
  //     2. Put the new byte in the now-empty bottom 8 bits.
  //
  // After five repetitions, all five bytes have been assembled.
  for (int i = 0; i < 5; i++) {

    value = (value << 8) | Wire.read();
  }

  return value;
}


// ============================================================================
// SIGN-EXTEND A 20-BIT VALUE
// ============================================================================
//
// CURRENT can be positive or negative.
//
// The INA stores it as a signed 20-bit two's-complement number.
//
// ESP32 normally works with signed 32-bit integers, so we convert the INA's
// unusual 20-bit signed value into a normal int32_t.

int32_t signExtend20(uint32_t value) {

  // Keep only the lower 20 bits.
  value &= 0xFFFFF;

  // Bit 19 is the sign bit.
  //
  // If it is 1, the value is negative.
  if (value & 0x80000) {

    // Fill the upper unused bits with 1s.
    value |= 0xFFF00000;
  }

  return (int32_t)value;
}


// ============================================================================
// SIGN-EXTEND A 40-BIT VALUE
// ============================================================================
//
// CHARGE can also be negative because current can flow backwards.
//
// The INA uses a signed 40-bit number.
//
// We convert that to a standard signed 64-bit integer.

int64_t signExtend40(uint64_t value) {

  // Keep only 40 bits.
  value &= 0xFFFFFFFFFFULL;

  // Bit 39 is the sign bit.
  if (value & 0x8000000000ULL) {

    // Fill all the unused upper bits with 1s.
    value |= 0xFFFFFF0000000000ULL;
  }

  return (int64_t)value;
}


// ============================================================================
// RESET INA228 ENERGY AND CHARGE ACCUMULATORS
// ============================================================================

void resetAccumulators() {

  // Read the existing CONFIG register.
  uint16_t config = readRegister16(REG_CONFIG);

  // Bit 14 is called RSTACC.
  //
  // OR this mask into CONFIG to temporarily set that bit to 1.
  config |= (1 << 14);

  // Writing RSTACC = 1 clears ENERGY and CHARGE.
  writeRegister16(REG_CONFIG, config);
}


// ============================================================================
// SETUP
// ============================================================================

void setup() {

  Serial.begin(115200);

  delay(2000);

  // XIAO ESP32-C3:
  //
  // D4 = SDA
  // D5 = SCL
  Wire.begin(D4, D5);

  Serial.println();
  Serial.println("INA228 power / energy / temperature diagnostic");
  Serial.println("================================================");


  // ==========================================================================
  // CONFIGURE THE INA228
  // ==========================================================================

  // Continuous bus + shunt + temperature conversion with 64x averaging.
  //
  // We are keeping the configuration that has already been stable for us.
  writeRegister16(REG_ADC_CONFIG, 0xFB6B);


  // Tell the INA228 about our shunt/current scaling.
  writeRegister16(REG_SHUNT_CAL, SHUNT_CAL_VALUE);


  // ==========================================================================
  // VERIFY OUR SETTINGS
  // ==========================================================================

  Serial.print("CONFIG:      0x");
  Serial.println(readRegister16(REG_CONFIG), HEX);

  Serial.print("ADC_CONFIG:  0x");
  Serial.println(readRegister16(REG_ADC_CONFIG), HEX);

  Serial.print("SHUNT_CAL:   ");
  Serial.println(readRegister16(REG_SHUNT_CAL));

  Serial.print("CURRENT_LSB: ");
  Serial.print(CURRENT_LSB * 1000000.0, 3);
  Serial.println(" uA/count");

  Serial.println();


  // ==========================================================================
  // START BOTH ACCUMULATION METHODS TOGETHER
  // ==========================================================================

  // Clear INA228 hardware ENERGY and CHARGE.
  resetAccumulators();

  // Immediately record our ESP32 software starting time.
  //
  // This makes the hardware and software accumulation periods line up much
  // more closely than they did in the previous sketch.
  previousMillis = millis();

  Serial.println("Accumulators reset. Test begins now.");
  Serial.println();
}


// ============================================================================
// MAIN LOOP
// ============================================================================

void loop() {

  // ==========================================================================
  // 1. CALCULATE ELAPSED TIME
  // ==========================================================================

  uint32_t currentMillis = millis();

  uint32_t elapsedMillis =
      currentMillis - previousMillis;

  previousMillis = currentMillis;


  // millis() gives milliseconds.
  //
  // Energy calculations use hours.
  //
  // 1 hour = 3,600,000 ms.
  double elapsedHours =
      elapsedMillis / 3600000.0;


  // ==========================================================================
  // 2. READ BUS VOLTAGE
  // ==========================================================================

  uint32_t rawBusRegister =
      readRegister24(REG_VBUS);

  // VBUS is a 20-bit result stored in bits 23 through 4.
  uint32_t rawBus =
      rawBusRegister >> 4;

  // Each VBUS ADC count is 195.3125 microvolts.
  double busVoltage =
      rawBus * 195.3125e-6;


  // ==========================================================================
  // 3. READ CURRENT
  // ==========================================================================

  uint32_t rawCurrentRegister =
      readRegister24(REG_CURRENT);

  // CURRENT is also stored in bits 23:4.
  uint32_t rawCurrentUnsigned =
      rawCurrentRegister >> 4;

  int32_t rawCurrent =
      signExtend20(rawCurrentUnsigned);

  // Convert counts into actual amps.
  double current =
      rawCurrent * CURRENT_LSB;


  // ==========================================================================
  // 4. READ INA228 POWER REGISTER
  // ==========================================================================

  uint32_t rawPower =
      readRegister24(REG_POWER);

  // According to the INA228 datasheet:
  //
  // Power [W] =
  //
  //     POWER register × 3.2 × CURRENT_LSB
  //
  // This is the power value calculated internally by the INA228.
  double powerFromRegister =
      rawPower * 3.2 * CURRENT_LSB;


  // ==========================================================================
  // 5. CALCULATE POWER OURSELVES
  // ==========================================================================

  // Completely independently of the INA POWER register, use:
  //
  //     P = V × I
  //
  // We already have voltage and current.
  //
  // volts × amps = watts
  double powerFromVI =
      busVoltage * current;


  // ==========================================================================
  // 6. READ TEMPERATURE
  // ==========================================================================

  // DIETEMP is a 16-bit signed value.
  //
  // Because readRegister16() returns uint16_t, explicitly cast it to int16_t.
  //
  // int16_t means:
  //
  //     signed 16-bit integer
  //
  // so negative temperatures can be represented.
  int16_t rawTemperature =
      (int16_t)readRegister16(REG_DIETEMP);


  // Each temperature count represents:
  //
  //     7.8125 millidegrees Celsius
  //
  // which is:
  //
  //     0.0078125 °C
  double temperatureC =
      rawTemperature * 0.0078125;


  // ==========================================================================
  // 7. SOFTWARE CHARGE INTEGRATION
  // ==========================================================================

  // amps × hours = amp-hours
  softwareAmpHours +=
      current * elapsedHours;


  // ==========================================================================
  // 8. SOFTWARE ENERGY METHOD A: INA POWER REGISTER
  // ==========================================================================

  // watts × hours = watt-hours
  softwareWhFromPowerRegister +=
      powerFromRegister * elapsedHours;


  // ==========================================================================
  // 9. SOFTWARE ENERGY METHOD B: OUR V × I CALCULATION
  // ==========================================================================

  softwareWhFromVI +=
      powerFromVI * elapsedHours;


  // ==========================================================================
  // 10. READ HARDWARE CHARGE ACCUMULATOR
  // ==========================================================================

  uint64_t rawChargeUnsigned =
      readRegister40(REG_CHARGE);

  int64_t rawCharge =
      signExtend40(rawChargeUnsigned);


  // INA CHARGE scaling gives us coulombs.
  double chargeCoulombs =
      rawCharge * CURRENT_LSB;


  // 3600 coulombs = 1 Ah.
  double hardwareAmpHours =
      chargeCoulombs / 3600.0;


  // ==========================================================================
  // 11. READ HARDWARE ENERGY ACCUMULATOR
  // ==========================================================================

  uint64_t rawEnergy =
      readRegister40(REG_ENERGY);


  // INA228 datasheet:
  //
  // Energy [J] =
  //
  //     ENERGY × 16 × 3.2 × CURRENT_LSB
  //
  // The ENERGY register therefore represents joules after scaling.
  double hardwareEnergyJoules =
      rawEnergy * 16.0 * 3.2 * CURRENT_LSB;


  // Convert joules into watt-hours.
  //
  //     1 Wh = 3600 J
  double hardwareWattHours =
      hardwareEnergyJoules / 3600.0;


  // ==========================================================================
  // 12. CALCULATE DIFFERENCES
  // ==========================================================================

  // Hardware charge minus software charge.
  double chargeDifference =
      hardwareAmpHours - softwareAmpHours;


  // Hardware ENERGY minus software integration of POWER register.
  double energyDifferencePower =
      hardwareWattHours - softwareWhFromPowerRegister;


  // Hardware ENERGY minus software integration of V × I.
  double energyDifferenceVI =
      hardwareWattHours - softwareWhFromVI;


  // Difference between the TWO instantaneous power calculations.
  double instantaneousPowerDifference =
      powerFromRegister - powerFromVI;


  // ==========================================================================
  // 13. PRINT INSTANTANEOUS MEASUREMENTS
  // ==========================================================================

  Serial.println("------------------------------------------------------------");

  Serial.print("Voltage: ");
  Serial.print(busVoltage, 4);
  Serial.print(" V");

  Serial.print("    Current: ");
  Serial.print(current * 1000.0, 3);
  Serial.print(" mA");

  Serial.print("    Temp: ");
  Serial.print(temperatureC, 2);
  Serial.println(" C");


  // ==========================================================================
  // 14. PRINT POWER COMPARISON
  // ==========================================================================

  Serial.print("Power REG: ");
  Serial.print(powerFromRegister * 1000.0, 3);
  Serial.print(" mW");

  Serial.print("    V*I: ");
  Serial.print(powerFromVI * 1000.0, 3);
  Serial.print(" mW");

  Serial.print("    Diff: ");
  Serial.print(instantaneousPowerDifference * 1000.0, 3);
  Serial.println(" mW");


  // ==========================================================================
  // 15. PRINT CHARGE COMPARISON
  // ==========================================================================

  Serial.print("Charge HW: ");
  Serial.print(hardwareAmpHours * 1000.0, 4);
  Serial.print(" mAh");

  Serial.print("    SW: ");
  Serial.print(softwareAmpHours * 1000.0, 4);
  Serial.print(" mAh");

  Serial.print("    Diff: ");
  Serial.print(chargeDifference * 1000.0, 4);
  Serial.println(" mAh");


  // ==========================================================================
  // 16. PRINT ENERGY COMPARISON
  // ==========================================================================

  Serial.print("Energy HW:       ");
  Serial.print(hardwareWattHours * 1000.0, 4);
  Serial.println(" mWh");


  Serial.print("Energy SW POWER: ");
  Serial.print(softwareWhFromPowerRegister * 1000.0, 4);
  Serial.print(" mWh");

  Serial.print("    Diff: ");
  Serial.print(energyDifferencePower * 1000.0, 4);
  Serial.println(" mWh");


  Serial.print("Energy SW V*I:   ");
  Serial.print(softwareWhFromVI * 1000.0, 4);
  Serial.print(" mWh");

  Serial.print("    Diff: ");
  Serial.print(energyDifferenceVI * 1000.0, 4);
  Serial.println(" mWh");

  Serial.println();


  // Wait two seconds before displaying another snapshot.
  //
  // Remember: the INA228's hardware ENERGY and CHARGE accumulation continues
  // internally during this delay.
  delay(2000);
}