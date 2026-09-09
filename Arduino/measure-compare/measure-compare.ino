#include <Wire.h>

constexpr uint8_t INA228_ADDRESS = 0x40;

constexpr uint8_t REG_CONFIG = 0x00;
constexpr uint8_t REG_VSHUNT = 0x04;
constexpr uint8_t REG_VBUS = 0x05;

constexpr double SHUNT_OHMS = 0.015;

// ---------- I2C helpers ----------

uint16_t readRegister16(uint8_t reg) {
  Wire.beginTransmission(INA228_ADDRESS);
  Wire.write(reg);

  if (Wire.endTransmission(false) != 0) {
    Serial.println("ERROR selecting 16-bit register");
    return 0;
  }

  if (Wire.requestFrom(INA228_ADDRESS, (uint8_t)2) != 2) {
    Serial.println("ERROR reading 16-bit register");
    return 0;
  }

  return ((uint16_t)Wire.read() << 8) |
         (uint16_t)Wire.read();
}

uint32_t readRegister24(uint8_t reg) {
  Wire.beginTransmission(INA228_ADDRESS);
  Wire.write(reg);

  if (Wire.endTransmission(false) != 0) {
    Serial.println("ERROR selecting 24-bit register");
    return 0;
  }

  if (Wire.requestFrom(INA228_ADDRESS, (uint8_t)3) != 3) {
    Serial.println("ERROR reading 24-bit register");
    return 0;
  }

  uint32_t value = 0;

  value |= ((uint32_t)Wire.read() << 16);
  value |= ((uint32_t)Wire.read() << 8);
  value |= ((uint32_t)Wire.read());

  return value;
}

// Convert 20-bit two's-complement value to signed integer.
int32_t signExtend20(uint32_t value) {
  value &= 0xFFFFF;

  if (value & 0x80000) {
    value |= 0xFFF00000;
  }

  return (int32_t)value;
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  Wire.begin(D4, D5);

  Serial.println();
  Serial.println("INA228 shunt current test");
  Serial.println("-------------------------");

  // Read config so we know which shunt ADC range is active.
  uint16_t config = readRegister16(REG_CONFIG);

  Serial.print("CONFIG register: 0x");
  Serial.println(config, HEX);
}

void loop() {

  // ----- Shunt voltage -----

  uint32_t rawShunt24 = readRegister24(REG_VSHUNT);

  // Measurement occupies bits 23:4.
  int32_t rawShunt20 = signExtend20(rawShunt24 >> 4);

  // Default ADCRANGE = 0:
  // 312.5 nV per LSB
  double shuntVoltage =
      rawShunt20 * 312.5e-9;

  // Ohm's law:
  // I = V / R
  double current =
      shuntVoltage / SHUNT_OHMS;

  // ----- Bus voltage -----

  uint32_t rawBus24 = readRegister24(REG_VBUS);

  uint32_t rawBus20 =
      (rawBus24 >> 4) & 0xFFFFF;

  double busVoltage =
      rawBus20 * 195.3125e-6;

  // ----- Output -----

  Serial.print("Shunt: ");
  Serial.print(shuntVoltage * 1000.0, 4);
  Serial.print(" mV");

  Serial.print("    Current: ");
  Serial.print(current * 1000.0, 3);
  Serial.print(" mA");

  Serial.print("    Bus: ");
  Serial.print(busVoltage, 4);
  Serial.println(" V");

  delay(1000);
}