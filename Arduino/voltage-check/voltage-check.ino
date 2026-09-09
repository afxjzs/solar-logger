#include <Wire.h>

constexpr uint8_t INA228_ADDRESS = 0x40;

// INA228 register addresses
constexpr uint8_t REG_VBUS = 0x05;

// Read a 24-bit register
uint32_t readRegister24(uint8_t reg) {
  Wire.beginTransmission(INA228_ADDRESS);
  Wire.write(reg);

  if (Wire.endTransmission(false) != 0) {
    Serial.println("ERROR: register select failed");
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

void setup() {
  Serial.begin(115200);
  delay(2000);

  Wire.begin(D4, D5);

  Serial.println();
  Serial.println("INA228 bus voltage test");
  Serial.println("-----------------------");
}

void loop() {
  uint32_t raw = readRegister24(REG_VBUS);

  // INA228 VBUS register uses bits 23:4.
  raw >>= 4;

  // Datasheet bus-voltage LSB = 195.3125 uV
  double voltage = raw * 195.3125e-6;

  Serial.print("Bus voltage: ");
  Serial.print(voltage, 6);
  Serial.println(" V");

  delay(1000);
}