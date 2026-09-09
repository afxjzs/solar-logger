#include <Wire.h>

constexpr uint8_t INA228_ADDRESS = 0x40;

// INA228 registers
constexpr uint8_t REG_MANUFACTURER_ID = 0x3E;
constexpr uint8_t REG_DEVICE_ID       = 0x3F;

uint16_t readRegister16(uint8_t reg) {
  Wire.beginTransmission(INA228_ADDRESS);
  Wire.write(reg);

  if (Wire.endTransmission(false) != 0) {
    Serial.println("ERROR: Could not select INA228 register.");
    return 0xFFFF;
  }

  if (Wire.requestFrom(INA228_ADDRESS, (uint8_t)2) != 2) {
    Serial.println("ERROR: INA228 did not return 2 bytes.");
    return 0xFFFF;
  }

  uint16_t value = ((uint16_t)Wire.read() << 8);
  value |= Wire.read();

  return value;
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("INA228 identity test");
  Serial.println("--------------------");

  // XIAO ESP32-C3:
  // D4 = SDA
  // D5 = SCL
  Wire.begin(D4, D5);

  uint16_t manufacturerID = readRegister16(REG_MANUFACTURER_ID);
  uint16_t deviceID = readRegister16(REG_DEVICE_ID);

  Serial.print("Manufacturer ID: 0x");
  Serial.println(manufacturerID, HEX);

  Serial.print("Device ID:       0x");
  Serial.println(deviceID, HEX);

  Serial.println();

  if (manufacturerID == 0x5449) {
    Serial.println("PASS: Manufacturer is Texas Instruments (\"TI\").");
  } else {
    Serial.println("FAIL: Unexpected manufacturer ID.");
  }

  // INA228 device ID is 0x228 in bits 15:4.
  uint16_t deviceNumber = deviceID >> 4;
  uint8_t revision = deviceID & 0x0F;

  Serial.print("Device number:   0x");
  Serial.println(deviceNumber, HEX);

  Serial.print("Revision:        ");
  Serial.println(revision);

  if (deviceNumber == 0x228) {
    Serial.println("PASS: Device identifies as INA228.");
  } else {
    Serial.println("FAIL: Device does not identify as INA228.");
  }
}

void loop() {
  // Nothing needed here.
}