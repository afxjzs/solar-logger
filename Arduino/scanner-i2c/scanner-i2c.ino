#include <Wire.h>

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("I2C scanner starting...");

  Wire.begin(D4, D5); // XIAO ESP32-C3: D4 = SDA, D5 = SCL
}

void loop() {
  byte error;
  int found = 0;

  Serial.println("Scanning...");

  for (byte address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    error = Wire.endTransmission();

    if (error == 0) {
      Serial.print("I2C device found at 0x");
      if (address < 16) {
        Serial.print("0");
      }
      Serial.println(address, HEX);
      found++;
    }
  }

  if (found == 0) {
    Serial.println("No I2C devices found.");
  } else {
    Serial.print("Found ");
    Serial.print(found);
    Serial.println(" device(s).");
  }

  Serial.println();
  delay(3000);
}