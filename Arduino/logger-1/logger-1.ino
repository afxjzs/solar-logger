#include <Wire.h>

// ============================================================================
// INA228 CONNECTION / REGISTER SETTINGS
// ============================================================================

// I2C address of our INA228 breakout.
// We already confirmed this with the I2C scanner.
constexpr uint8_t INA228_ADDRESS = 0x40;

// INA228 register addresses.
constexpr uint8_t REG_CONFIG     = 0x00;
constexpr uint8_t REG_ADC_CONFIG = 0x01;
constexpr uint8_t REG_VSHUNT     = 0x04;
constexpr uint8_t REG_VBUS       = 0x05;


// ============================================================================
// OUR BOARD-SPECIFIC CALIBRATION
// ============================================================================

// The physical shunt is marked R015, which nominally means 0.015 ohms.
//
// Our bench calibration showed that this particular breakout behaves like
// approximately 0.01562 ohms.
//
// Using the calibrated value makes the INA228 current reading agree closely
// with our DMM + known-resistor measurement.
constexpr double SHUNT_OHMS = 0.01562;


// ============================================================================
// RUNNING TOTALS
// ============================================================================

// These variables hold the accumulated charge and energy.
//
// We use double instead of float because these values may accumulate for a
// long time and we want to minimize rounding error.
//
// ampHours:
//     total electrical charge that has flowed through the shunt
//
// wattHours:
//     total electrical energy that has flowed through the shunt
double ampHours = 0.0;
double wattHours = 0.0;


// This stores the time of the previous measurement.
//
// millis() returns the number of milliseconds since the ESP32 started.
//
// We use this so we know exactly how much time passed between samples.
uint32_t previousMillis = 0;


// ============================================================================
// READ A 16-BIT REGISTER
// ============================================================================

uint16_t readRegister16(uint8_t reg) {

  // Start an I2C transaction with the INA228.
  Wire.beginTransmission(INA228_ADDRESS);

  // Tell the INA228 which register we want.
  Wire.write(reg);

  // End the write portion, but keep control of the I2C bus because we're
  // immediately going to read from the same device.
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

  return value;
}


// ============================================================================
// WRITE A 16-BIT REGISTER
// ============================================================================

void writeRegister16(uint8_t reg, uint16_t value) {

  // Start an I2C write.
  Wire.beginTransmission(INA228_ADDRESS);

  // Tell the INA228 which register we want to modify.
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

  // Start talking to the INA228.
  Wire.beginTransmission(INA228_ADDRESS);

  // Select the register.
  Wire.write(reg);

  // Finish the register-selection portion while keeping the bus active.
  if (Wire.endTransmission(false) != 0) {
    Serial.println("ERROR: could not select 24-bit register");
    return 0;
  }

  // Ask the INA228 for three bytes.
  if (Wire.requestFrom(INA228_ADDRESS, (uint8_t)3) != 3) {
    Serial.println("ERROR: expected 3 bytes");
    return 0;
  }

  // Assemble the three incoming bytes into one 32-bit variable.
  //
  // Only 24 bits are used, but a 32-bit type makes the math easier.
  uint32_t value = 0;

  value |= ((uint32_t)Wire.read() << 16);
  value |= ((uint32_t)Wire.read() << 8);
  value |= ((uint32_t)Wire.read());

  return value;
}


// ============================================================================
// SIGN-EXTEND A 20-BIT VALUE
// ============================================================================

int32_t signExtend20(uint32_t value) {

  // Keep only the lower 20 bits.
  value &= 0xFFFFF;

  // Bit 19 is the sign bit.
  //
  // If it is 1, this is a negative number.
  if (value & 0x80000) {

    // Fill the unused upper bits with 1s so the ESP32 correctly interprets
    // this as a negative 32-bit signed integer.
    value |= 0xFFF00000;
  }

  return (int32_t)value;
}


// ============================================================================
// SETUP
// ============================================================================

void setup() {

  // Start the USB serial connection.
  Serial.begin(115200);

  // Give the serial connection time to initialize.
  delay(2000);

  // Start I2C.
  //
  // XIAO ESP32-C3:
  // D4 = SDA
  // D5 = SCL
  Wire.begin(D4, D5);

  Serial.println();
  Serial.println("INA228 energy / charge integration test");
  Serial.println("---------------------------------------");

  // Configure the INA228 ADC.
  //
  // 0xFB6B keeps the device in continuous measurement mode and uses
  // 64-sample averaging.
  writeRegister16(REG_ADC_CONFIG, 0xFB6B);

  // Read the configuration registers back so we can verify them.
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

  // Save the current time as our starting point.
  //
  // The first loop iteration will measure the amount of time that elapsed
  // after this point.
  previousMillis = millis();
}


// ============================================================================
// MAIN LOOP
// ============================================================================

void loop() {

  // --------------------------------------------------------------------------
  // MEASURE ELAPSED TIME
  // --------------------------------------------------------------------------

  // Get the current uptime in milliseconds.
  uint32_t currentMillis = millis();

  // Subtract the time of the previous sample.
  //
  // Because uint32_t arithmetic wraps correctly, this still works when
  // millis() eventually rolls over.
  uint32_t elapsedMillis = currentMillis - previousMillis;

  // Save this timestamp so the next loop can calculate its elapsed interval.
  previousMillis = currentMillis;

  // Convert milliseconds into hours.
  //
  // There are:
  //
  //     1000 milliseconds per second
  //     60 seconds per minute
  //     60 minutes per hour
  //
  // therefore:
  //
  //     3,600,000 milliseconds per hour
  double elapsedHours = elapsedMillis / 3600000.0;


  // --------------------------------------------------------------------------
  // READ SHUNT VOLTAGE
  // --------------------------------------------------------------------------

  // Read the raw 24-bit VSHUNT register.
  uint32_t rawShuntRegister = readRegister24(REG_VSHUNT);

  // The actual measurement is stored in bits 23:4.
  uint32_t rawShuntUnsigned = rawShuntRegister >> 4;

  // VSHUNT can be positive or negative.
  //
  // Convert the 20-bit two's-complement measurement into a signed integer.
  int32_t rawShunt = signExtend20(rawShuntUnsigned);

  // With ADCRANGE = 0, one ADC count represents:
  //
  //     312.5 nanovolts
  //
  // Convert the ADC count into volts.
  double shuntVoltage = rawShunt * 312.5e-9;


  // --------------------------------------------------------------------------
  // CALCULATE CURRENT
  // --------------------------------------------------------------------------

  // Ohm's law:
  //
  //     I = V / R
  //
  // shuntVoltage is in volts.
  // SHUNT_OHMS is in ohms.
  //
  // Therefore current is in amps.
  double current = shuntVoltage / SHUNT_OHMS;


  // --------------------------------------------------------------------------
  // READ BUS VOLTAGE
  // --------------------------------------------------------------------------

  // Read the raw VBUS register.
  uint32_t rawBusRegister = readRegister24(REG_VBUS);

  // The actual ADC measurement occupies bits 23:4.
  uint32_t rawBus = rawBusRegister >> 4;

  // Each ADC count represents:
  //
  //     195.3125 microvolts
  //
  // Convert the ADC count into volts.
  double busVoltage = rawBus * 195.3125e-6;


  // --------------------------------------------------------------------------
  // CALCULATE INSTANTANEOUS POWER
  // --------------------------------------------------------------------------

  // Electrical power:
  //
  //     P = V × I
  //
  // volts × amps = watts
  double power = busVoltage * current;


  // --------------------------------------------------------------------------
  // INTEGRATE CHARGE
  // --------------------------------------------------------------------------

  // Current tells us how fast charge is flowing right now.
  //
  // To determine how much charge flowed during this time interval:
  //
  //     charge = current × time
  //
  // amps × hours = amp-hours
  //
  // Example:
  //
  //     1 amp flowing for 1 hour = 1 Ah
  //
  // We add the tiny amount from this loop to our running total.
  ampHours += current * elapsedHours;


  // --------------------------------------------------------------------------
  // INTEGRATE ENERGY
  // --------------------------------------------------------------------------

  // Power tells us how fast energy is flowing right now.
  //
  // To determine how much energy flowed during this interval:
  //
  //     energy = power × time
  //
  // watts × hours = watt-hours
  //
  // Again, we add this interval's contribution to the running total.
  wattHours += power * elapsedHours;


  // --------------------------------------------------------------------------
  // CALCULATE TOTAL RUN TIME
  // --------------------------------------------------------------------------

  // millis() gives us total milliseconds since startup.
  //
  // Convert that into seconds so the output is easier to understand.
  double runtimeSeconds = currentMillis / 1000.0;


  // --------------------------------------------------------------------------
  // PRINT CURRENT MEASUREMENTS
  // --------------------------------------------------------------------------

  Serial.print("Time: ");
  Serial.print(runtimeSeconds, 1);
  Serial.print(" s");

  Serial.print("    Voltage: ");
  Serial.print(busVoltage, 4);
  Serial.print(" V");

  Serial.print("    Current: ");
  Serial.print(current * 1000.0, 3);
  Serial.print(" mA");

  Serial.print("    Power: ");
  Serial.print(power * 1000.0, 3);
  Serial.print(" mW");


  // --------------------------------------------------------------------------
  // PRINT ACCUMULATED TOTALS
  // --------------------------------------------------------------------------

  Serial.print("    Charge: ");

  // ampHours is stored internally in Ah.
  //
  // Multiply by 1000 so the display is easier to read during this small
  // bench test.
  Serial.print(ampHours * 1000.0, 4);
  Serial.print(" mAh");

  Serial.print("    Energy: ");

  // wattHours is stored internally in Wh.
  //
  // Multiply by 1000 so we display mWh.
  Serial.print(wattHours * 1000.0, 4);
  Serial.println(" mWh");


  // Wait two seconds before the next measurement.
  delay(2000);
}