
#include <Wire.h>

// ============================================================================
// INA228 CONNECTION / REGISTER SETTINGS
// ============================================================================

// I2C address of our INA228 breakout.
//
// We already confirmed this with the I2C scanner.
constexpr uint8_t INA228_ADDRESS = 0x40;

// INA228 register addresses.
//
// These come directly from the INA228 register map.
constexpr uint8_t REG_CONFIG     = 0x00;
constexpr uint8_t REG_ADC_CONFIG = 0x01;
constexpr uint8_t REG_VSHUNT     = 0x04;
constexpr uint8_t REG_VBUS       = 0x05;


// ============================================================================
// OUR BOARD-SPECIFIC CALIBRATION
// ============================================================================

// The resistor on the breakout is marked:
//
//     R015
//
// which nominally means:
//
//     0.015 ohms
//
// But our bench measurements showed that the effective resistance seen by
// the INA228 is closer to:
//
//     0.01562 ohms
//
// We determined this by measuring:
//
//     resistor voltage / resistor resistance = actual current
//
// and then:
//
//     INA shunt voltage / actual current = effective shunt resistance
//
// This value applies to THIS breakout board.
constexpr double SHUNT_OHMS = 0.01562;


// ============================================================================
// READ A 16-BIT REGISTER
// ============================================================================

uint16_t readRegister16(uint8_t reg) {

  // Begin an I2C transaction with the INA228.
  Wire.beginTransmission(INA228_ADDRESS);

  // Tell the INA228 which register we want to read.
  Wire.write(reg);

  // Send the register address, but keep control of the I2C bus because
  // we're about to immediately read from the chip.
  if (Wire.endTransmission(false) != 0) {
    Serial.println("ERROR: could not select 16-bit register");
    return 0;
  }

  // Ask the INA228 for two bytes.
  if (Wire.requestFrom(INA228_ADDRESS, (uint8_t)2) != 2) {
    Serial.println("ERROR: expected 2 bytes");
    return 0;
  }

  // The INA228 sends the most-significant byte first.
  uint16_t value = ((uint16_t)Wire.read() << 8);

  // Add the least-significant byte.
  value |= (uint16_t)Wire.read();

  // Return the complete 16-bit value.
  return value;
}


// ============================================================================
// WRITE A 16-BIT REGISTER
// ============================================================================

void writeRegister16(uint8_t reg, uint16_t value) {

  // Begin talking to the INA228.
  Wire.beginTransmission(INA228_ADDRESS);

  // Tell it which register we want to modify.
  Wire.write(reg);

  // Send the high byte of our 16-bit value.
  Wire.write((uint8_t)(value >> 8));

  // Send the low byte.
  Wire.write((uint8_t)(value & 0xFF));

  // Finish the I2C transaction.
  Wire.endTransmission();
}


// ============================================================================
// READ A 24-BIT REGISTER
// ============================================================================

uint32_t readRegister24(uint8_t reg) {

  // Begin communicating with the INA228.
  Wire.beginTransmission(INA228_ADDRESS);

  // Select the register.
  Wire.write(reg);

  // End this part of the transaction while keeping control of the bus.
  if (Wire.endTransmission(false) != 0) {
    Serial.println("ERROR: could not select 24-bit register");
    return 0;
  }

  // Ask the INA228 for three bytes.
  if (Wire.requestFrom(INA228_ADDRESS, (uint8_t)3) != 3) {
    Serial.println("ERROR: expected 3 bytes");
    return 0;
  }

  // Start with an empty 32-bit integer.
  //
  // We only need 24 bits, but using a 32-bit variable makes the math easier.
  uint32_t value = 0;

  // Read the most-significant byte.
  value |= ((uint32_t)Wire.read() << 16);

  // Read the middle byte.
  value |= ((uint32_t)Wire.read() << 8);

  // Read the least-significant byte.
  value |= ((uint32_t)Wire.read());

  // Return the complete 24-bit register value.
  return value;
}


// ============================================================================
// CONVERT A SIGNED 20-BIT NUMBER INTO A NORMAL SIGNED INTEGER
// ============================================================================

int32_t signExtend20(uint32_t value) {

  // The INA228 measurement occupies only 20 bits.
  //
  // This mask discards anything outside those 20 bits.
  value &= 0xFFFFF;

  // Bit 19 is the sign bit.
  //
  // If bit 19 is 1, the measurement is negative.
  if (value & 0x80000) {

    // Fill the unused upper bits with 1s so the ESP32 understands that this
    // should be treated as a negative 32-bit integer.
    value |= 0xFFF00000;
  }

  return (int32_t)value;
}


// ============================================================================
// SETUP
// ============================================================================

void setup() {

  // Start the USB serial connection.
  //
  // This must match the baud rate selected in Arduino Serial Monitor.
  Serial.begin(115200);

  // Give USB serial a little time to initialize.
  delay(2000);

  // Start I2C communication.
  //
  // On the XIAO ESP32-C3:
  //
  // D4 = SDA
  // D5 = SCL
  Wire.begin(D4, D5);

  Serial.println();
  Serial.println("INA228 calibrated power monitor");
  Serial.println("--------------------------------");

  // Configure the INA228 ADC.
  //
  // 0xFB6B keeps the normal continuous measurement mode but changes averaging
  // to 64 samples.
  //
  // Averaging helps reduce small fluctuations in the ADC readings.
  writeRegister16(REG_ADC_CONFIG, 0xFB6B);

  // Read back the registers so we can make sure the chip accepted our setup.
  uint16_t config = readRegister16(REG_CONFIG);
  uint16_t adcConfig = readRegister16(REG_ADC_CONFIG);

  Serial.print("CONFIG:     0x");
  Serial.println(config, HEX);

  Serial.print("ADC_CONFIG: 0x");
  Serial.println(adcConfig, HEX);

  Serial.print("Calibrated shunt resistance: ");
  Serial.print(SHUNT_OHMS * 1000.0, 3);
  Serial.println(" mOhm");

  Serial.println();
}


// ============================================================================
// MAIN LOOP
// ============================================================================

void loop() {

  // --------------------------------------------------------------------------
  // READ SHUNT VOLTAGE
  // --------------------------------------------------------------------------

  // Read the complete 24-bit VSHUNT register.
  uint32_t rawShuntRegister = readRegister24(REG_VSHUNT);

  // The useful measurement is stored in bits 23:4.
  //
  // Shifting right by four removes the unused lower four bits.
  uint32_t rawShuntUnsigned = rawShuntRegister >> 4;

  // Shunt voltage can be positive OR negative.
  //
  // Positive current flows VIN+ -> VIN-.
  //
  // Negative current would mean current is flowing in the opposite direction.
  int32_t rawShunt = signExtend20(rawShuntUnsigned);

  // With ADCRANGE = 0, each VSHUNT ADC count represents:
  //
  //     312.5 nanovolts
  //
  // Convert the raw ADC count into volts.
  double shuntVoltage = rawShunt * 312.5e-9;


  // --------------------------------------------------------------------------
  // CALCULATE CURRENT
  // --------------------------------------------------------------------------

  // Ohm's law:
  //
  //     I = V / R
  //
  // We know the voltage across the shunt and our measured shunt resistance.
  double current = shuntVoltage / SHUNT_OHMS;


  // --------------------------------------------------------------------------
  // READ BUS VOLTAGE
  // --------------------------------------------------------------------------

  // Read the complete 24-bit VBUS register.
  uint32_t rawBusRegister = readRegister24(REG_VBUS);

  // As with VSHUNT, the measurement is in bits 23:4.
  uint32_t rawBus = rawBusRegister >> 4;

  // Each VBUS ADC count represents:
  //
  //     195.3125 microvolts
  //
  // Convert the raw ADC count into volts.
  double busVoltage = rawBus * 195.3125e-6;


  // --------------------------------------------------------------------------
  // CALCULATE POWER
  // --------------------------------------------------------------------------

  // Electrical power is:
  //
  //     P = V × I
  //
  // busVoltage is in volts.
  // current is in amps.
  //
  // The result is therefore watts.
  double power = busVoltage * current;


  // --------------------------------------------------------------------------
  // PRINT THE RESULTS
  // --------------------------------------------------------------------------

  Serial.print("Voltage: ");
  Serial.print(busVoltage, 4);
  Serial.print(" V");

  Serial.print("    Current: ");
  Serial.print(current * 1000.0, 3);
  Serial.print(" mA");

  Serial.print("    Power: ");
  Serial.print(power * 1000.0, 3);
  Serial.println(" mW");


  // Wait two seconds before taking the next displayed measurement.
  delay(2000);
}