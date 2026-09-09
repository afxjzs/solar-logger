#include <Wire.h>

// -----------------------------------------------------------------------------
// INA228 I2C configuration
// -----------------------------------------------------------------------------

// The INA228 is at address 0x40 with the address pins configured as they are
// on our breakout board.
constexpr uint8_t INA228_ADDRESS = 0x40;

// Register addresses from the INA228 datasheet.
constexpr uint8_t REG_CONFIG     = 0x00;
constexpr uint8_t REG_ADC_CONFIG = 0x01;
constexpr uint8_t REG_VSHUNT     = 0x04;
constexpr uint8_t REG_VBUS       = 0x05;

// The physical resistor marked "R015" on the breakout is nominally 0.015 ohms.
constexpr double SHUNT_OHMS = 0.015;


// -----------------------------------------------------------------------------
// Read a 16-bit INA228 register
// -----------------------------------------------------------------------------

uint16_t readRegister16(uint8_t reg) {

  // Start talking to the INA228.
  Wire.beginTransmission(INA228_ADDRESS);

  // Tell the INA228 which register we want to read.
  Wire.write(reg);

  // Finish the write, but keep control of the I2C bus because a read follows.
  if (Wire.endTransmission(false) != 0) {
    Serial.println("ERROR: could not select 16-bit register");
    return 0;
  }

  // Ask the INA228 to send us two bytes.
  if (Wire.requestFrom(INA228_ADDRESS, (uint8_t)2) != 2) {
    Serial.println("ERROR: expected 2 bytes");
    return 0;
  }

  // The INA228 sends the most-significant byte first.
  uint16_t value = ((uint16_t)Wire.read() << 8);

  // Add the least-significant byte.
  value |= (uint16_t)Wire.read();

  return value;
}


// -----------------------------------------------------------------------------
// Write a 16-bit INA228 register
// -----------------------------------------------------------------------------

void writeRegister16(uint8_t reg, uint16_t value) {

  // Start an I2C write to the INA228.
  Wire.beginTransmission(INA228_ADDRESS);

  // First byte tells the INA which register we want to change.
  Wire.write(reg);

  // Send the high byte of the 16-bit value.
  Wire.write((uint8_t)(value >> 8));

  // Send the low byte.
  Wire.write((uint8_t)(value & 0xFF));

  // Finish the transaction.
  Wire.endTransmission();
}


// -----------------------------------------------------------------------------
// Read a 24-bit INA228 register
// -----------------------------------------------------------------------------

uint32_t readRegister24(uint8_t reg) {

  // Select the register.
  Wire.beginTransmission(INA228_ADDRESS);
  Wire.write(reg);

  if (Wire.endTransmission(false) != 0) {
    Serial.println("ERROR: could not select 24-bit register");
    return 0;
  }

  // VSHUNT and VBUS are each stored in three bytes.
  if (Wire.requestFrom(INA228_ADDRESS, (uint8_t)3) != 3) {
    Serial.println("ERROR: expected 3 bytes");
    return 0;
  }

  uint32_t value = 0;

  // Assemble the three incoming bytes into one 24-bit number.
  value |= ((uint32_t)Wire.read() << 16);
  value |= ((uint32_t)Wire.read() << 8);
  value |= ((uint32_t)Wire.read());

  return value;
}


// -----------------------------------------------------------------------------
// Convert a 20-bit two's-complement number into a normal signed integer
// -----------------------------------------------------------------------------

int32_t signExtend20(uint32_t value) {

  // Keep only the lower 20 bits.
  value &= 0xFFFFF;

  // Bit 19 is the sign bit.
  //
  // If it is set, this is a negative value. Fill the upper bits with 1s so
  // the ESP32 interprets it correctly as a signed 32-bit integer.
  if (value & 0x80000) {
    value |= 0xFFF00000;
  }

  return (int32_t)value;
}


// -----------------------------------------------------------------------------
// Arduino setup
// -----------------------------------------------------------------------------

void setup() {

  // Start the USB serial connection so we can see measurements.
  Serial.begin(115200);

  // Give the serial port a moment to initialize.
  delay(2000);

  // Start I2C.
  //
  // On the XIAO ESP32-C3:
  // D4 = SDA
  // D5 = SCL
  Wire.begin(D4, D5);

  Serial.println();
  Serial.println("INA228 precision comparison test");
  Serial.println("--------------------------------");


  // ---------------------------------------------------------------------------
  // Configure the ADC
  // ---------------------------------------------------------------------------
  //
  // INA228 reset value for ADC_CONFIG is 0xFB68.
  //
  // Bits 2:0 control averaging:
  //
  //   000 = 1 sample
  //   001 = 4 samples
  //   010 = 16 samples
  //   011 = 64 samples
  //
  // Changing the final bits from 000 to 011 gives:
  //
  //   0xFB68 -> 0xFB6B
  //
  // Everything else remains at its default:
  //   - continuous measurement
  //   - bus voltage enabled
  //   - shunt voltage enabled
  //   - temperature enabled
  //   - default conversion times
  //
  // Averaging should greatly reduce the small sample-to-sample noise we were
  // seeing on the breadboard.

  writeRegister16(REG_ADC_CONFIG, 0xFB6B);

  // Read the registers back so we can confirm the device accepted the setting.
  uint16_t config = readRegister16(REG_CONFIG);
  uint16_t adcConfig = readRegister16(REG_ADC_CONFIG);

  Serial.print("CONFIG:     0x");
  Serial.println(config, HEX);

  Serial.print("ADC_CONFIG: 0x");
  Serial.println(adcConfig, HEX);

  Serial.println();
}


// -----------------------------------------------------------------------------
// Main measurement loop
// -----------------------------------------------------------------------------

void loop() {

  // ---------------------------------------------------------------------------
  // Read shunt voltage
  // ---------------------------------------------------------------------------

  // Read the complete 24-bit VSHUNT register.
  uint32_t rawShuntRegister = readRegister24(REG_VSHUNT);

  // Bits 23:4 contain the actual 20-bit measurement.
  //
  // Bits 3:0 are reserved, so throw them away.
  uint32_t rawShunt20Unsigned = rawShuntRegister >> 4;

  // Shunt voltage can be positive or negative, so convert the 20-bit
  // two's-complement number into a signed integer.
  int32_t rawShunt20 = signExtend20(rawShunt20Unsigned);

  // CONFIG.ADCRANGE is 0 in our setup.
  //
  // According to the INA228 datasheet, each ADC count therefore represents:
  //
  //   312.5 nanovolts
  //
  // 312.5 nV = 312.5 × 10^-9 volts.
  double shuntVoltage = rawShunt20 * 312.5e-9;

  // Current follows Ohm's law:
  //
  //   current = voltage / resistance
  //
  // The breakout's shunt is marked R015, meaning nominally 0.015 ohms.
  double current = shuntVoltage / SHUNT_OHMS;


  // ---------------------------------------------------------------------------
  // Read bus voltage
  // ---------------------------------------------------------------------------

  // Read the complete 24-bit VBUS register.
  uint32_t rawBusRegister = readRegister24(REG_VBUS);

  // As with VSHUNT, the actual 20-bit measurement is stored in bits 23:4.
  uint32_t rawBus20 = rawBusRegister >> 4;

  // Each VBUS ADC count represents 195.3125 microvolts.
  //
  // 195.3125 uV = 195.3125 × 10^-6 volts.
  double busVoltage = rawBus20 * 195.3125e-6;


  // ---------------------------------------------------------------------------
  // Print everything
  // ---------------------------------------------------------------------------

  // Showing the raw counts lets us distinguish an ADC/input problem from a
  // software conversion problem.
  Serial.print("Raw VSHUNT: ");
  Serial.print(rawShunt20);

  Serial.print("    Raw VBUS: ");
  Serial.print(rawBus20);

  Serial.println();

  Serial.print("Shunt: ");
  Serial.print(shuntVoltage * 1000.0, 6);
  Serial.print(" mV");

  Serial.print("    Current: ");
  Serial.print(current * 1000.0, 3);
  Serial.print(" mA");

  Serial.print("    Bus: ");
  Serial.print(busVoltage, 6);
  Serial.println(" V");

  Serial.println();

  // Wait two seconds before the next displayed measurement.
  delay(2000);
}