#include <Wire.h>
#include <Preferences.h>

// -----------------------------------------------------------------------------
// BMW Solar Logger - development firmware
//
// Hardware:
//   Seeed Studio XIAO ESP32-C3
//   TI INA228 current / voltage / power monitor at I2C address 0x40
//
// Development philosophy:
//   - Stay awake and keep USB Serial available.
//   - Print what the firmware is doing.
//   - Surface errors instead of silently continuing.
//   - Measure one 60-second interval at a time.
//   - Keep cumulative charge and energy in NVS checkpoints.
//   - Emit one machine-readable CSV row per completed interval so the data can
//     be captured on the Mac and graphed later.
//
// Serial commands:
//   HELP       - show available commands
//   STATUS     - print current state
//   RESET YES  - zero interval/charge/energy totals, save zeroes to NVS, and
//                start a new clean measurement interval
// -----------------------------------------------------------------------------

// ----------------------------- I2C / INA228 ----------------------------------

constexpr uint8_t INA228_ADDRESS = 0x40;

constexpr uint8_t REG_CONFIG          = 0x00;
constexpr uint8_t REG_ADC_CONFIG      = 0x01;
constexpr uint8_t REG_SHUNT_CAL       = 0x02;
constexpr uint8_t REG_VSHUNT          = 0x04;
constexpr uint8_t REG_VBUS            = 0x05;
constexpr uint8_t REG_DIETEMP         = 0x06;
constexpr uint8_t REG_CURRENT         = 0x07;
constexpr uint8_t REG_POWER           = 0x08;
constexpr uint8_t REG_ENERGY          = 0x09;
constexpr uint8_t REG_CHARGE          = 0x0A;
constexpr uint8_t REG_MANUFACTURER_ID = 0x3E;
constexpr uint8_t REG_DEVICE_ID       = 0x3F;

// Known-good configuration from our bench calibration.
constexpr uint16_t ADC_CONFIG_VALUE = 0xFB6B;
constexpr uint16_t SHUNT_CAL_VALUE  = 819;

constexpr double SHUNT_OHMS = 0.01562;       // 15.62 mOhm effective shunt value
constexpr double CURRENT_LSB_A = 4.0e-6;     // 4 uA per CURRENT-register count
constexpr double VSHUNT_LSB_V  = 312.5e-9;   // ADCRANGE = 0
constexpr double VBUS_LSB_V    = 195.3125e-6;
constexpr double DIETEMP_LSB_C = 0.0078125;

// INA228 datasheet scaling derived from CURRENT_LSB.
constexpr double POWER_LSB_W  = 3.2 * CURRENT_LSB_A;
constexpr double ENERGY_LSB_J = 16.0 * POWER_LSB_W;
constexpr double CHARGE_LSB_C = CURRENT_LSB_A;

constexpr double VSHUNT_FULL_SCALE_V = 0.16384;  // ADCRANGE = 0

// ----------------------------- Logger timing --------------------------------

constexpr unsigned long MEASUREMENT_INTERVAL_MS = 60UL * 1000UL;
constexpr unsigned long HEARTBEAT_INTERVAL_MS   = 5UL * 1000UL;
constexpr uint32_t NVS_CHECKPOINT_EVERY_INTERVALS = 5;

// ----------------------------- Persistent state ------------------------------

Preferences preferences;
constexpr char NVS_NAMESPACE[] = "solarlog";
constexpr uint32_t NVS_SCHEMA_VERSION = 1;

uint32_t completedInterval = 0;
double runningCharge_mAh = 0.0;
double runningEnergy_mWh = 0.0;

// ----------------------------- Runtime state ---------------------------------

unsigned long intervalStartMs = 0;
unsigned long lastHeartbeatMs = 0;
String serialCommandBuffer;

struct SensorReading {
  double voltage_V;
  double current_mA;
  double power_mW;
  double temperature_C;
  int32_t rawVshuntCounts;
  double shuntVoltage_mV;
  double currentFromShunt_mA;
};

// -----------------------------------------------------------------------------
// Small integer helpers
// -----------------------------------------------------------------------------

int32_t signExtend20(uint32_t value) {
  value &= 0xFFFFF;

  if (value & 0x80000) {
    value |= 0xFFF00000;
  }

  return static_cast<int32_t>(value);
}

int64_t signExtend40(uint64_t value) {
  value &= 0xFFFFFFFFFFULL;

  if (value & 0x8000000000ULL) {
    value |= 0xFFFFFF0000000000ULL;
  }

  return static_cast<int64_t>(value);
}

// -----------------------------------------------------------------------------
// I2C register access
//
// Every function returns true on success and false on failure.
//
// This is important. If an I2C read fails, we do NOT want the firmware to
// silently return zero and make that zero look like a legitimate measurement.
// -----------------------------------------------------------------------------

bool readRegisterBytes(uint8_t reg, uint8_t *buffer, size_t length) {
  Wire.beginTransmission(INA228_ADDRESS);
  Wire.write(reg);

  uint8_t txResult = Wire.endTransmission(false);

  if (txResult != 0) {
    Serial.print("[ERROR] I2C address/write phase failed for register 0x");
    Serial.print(reg, HEX);
    Serial.print(". Wire error = ");
    Serial.println(txResult);
    return false;
  }

  size_t received =
      Wire.requestFrom(INA228_ADDRESS, static_cast<uint8_t>(length));

  if (received != length) {
    Serial.print("[ERROR] I2C short read for register 0x");
    Serial.print(reg, HEX);
    Serial.print(". Expected ");
    Serial.print(length);
    Serial.print(" byte(s), received ");
    Serial.println(received);
    return false;
  }

  for (size_t i = 0; i < length; i++) {
    if (!Wire.available()) {
      Serial.print(
          "[ERROR] I2C buffer unexpectedly empty while reading register 0x");
      Serial.println(reg, HEX);
      return false;
    }

    buffer[i] = Wire.read();
  }

  return true;
}

bool readRegister16(uint8_t reg, uint16_t &value) {
  uint8_t bytes[2];

  if (!readRegisterBytes(reg, bytes, sizeof(bytes))) {
    return false;
  }

  value =
      (static_cast<uint16_t>(bytes[0]) << 8) |
      bytes[1];

  return true;
}

bool writeRegister16(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(INA228_ADDRESS);
  Wire.write(reg);
  Wire.write(static_cast<uint8_t>(value >> 8));
  Wire.write(static_cast<uint8_t>(value & 0xFF));

  uint8_t result = Wire.endTransmission();

  if (result != 0) {
    Serial.print("[ERROR] I2C write failed for register 0x");
    Serial.print(reg, HEX);
    Serial.print(". Wire error = ");
    Serial.println(result);
    return false;
  }

  return true;
}

bool readRegister20Signed(uint8_t reg, int32_t &value) {
  uint8_t bytes[3];

  if (!readRegisterBytes(reg, bytes, sizeof(bytes))) {
    return false;
  }

  // INA228 20-bit measurement registers are left-justified inside 24 bits.
  uint32_t raw24 =
      (static_cast<uint32_t>(bytes[0]) << 16) |
      (static_cast<uint32_t>(bytes[1]) << 8) |
      bytes[2];

  value = signExtend20(raw24 >> 4);
  return true;
}

bool readRegister20Unsigned(uint8_t reg, uint32_t &value) {
  uint8_t bytes[3];

  if (!readRegisterBytes(reg, bytes, sizeof(bytes))) {
    return false;
  }

  uint32_t raw24 =
      (static_cast<uint32_t>(bytes[0]) << 16) |
      (static_cast<uint32_t>(bytes[1]) << 8) |
      bytes[2];

  value = raw24 >> 4;
  return true;
}

bool readRegister24Unsigned(uint8_t reg, uint32_t &value) {
  uint8_t bytes[3];

  if (!readRegisterBytes(reg, bytes, sizeof(bytes))) {
    return false;
  }

  value =
      (static_cast<uint32_t>(bytes[0]) << 16) |
      (static_cast<uint32_t>(bytes[1]) << 8) |
      bytes[2];

  return true;
}

bool readRegister40Unsigned(uint8_t reg, uint64_t &value) {
  uint8_t bytes[5];

  if (!readRegisterBytes(reg, bytes, sizeof(bytes))) {
    return false;
  }

  value = 0;

  for (uint8_t byte : bytes) {
    value = (value << 8) | byte;
  }

  return true;
}

bool readRegister40Signed(uint8_t reg, int64_t &value) {
  uint64_t raw;

  if (!readRegister40Unsigned(reg, raw)) {
    return false;
  }

  value = signExtend40(raw);
  return true;
}

// -----------------------------------------------------------------------------
// INA228 setup and measurement
// -----------------------------------------------------------------------------

bool verifyInaIdentity() {
  Serial.println("[INA228] Verifying device identity...");

  uint16_t manufacturerId;
  uint16_t deviceId;

  if (!readRegister16(REG_MANUFACTURER_ID, manufacturerId) ||
      !readRegister16(REG_DEVICE_ID, deviceId)) {
    Serial.println("[ERROR] Could not read INA228 identity registers.");
    return false;
  }

  Serial.print("[INA228] Manufacturer ID = 0x");
  Serial.println(manufacturerId, HEX);

  Serial.print("[INA228] Device ID       = 0x");
  Serial.println(deviceId, HEX);

  if (manufacturerId != 0x5449) {
    Serial.println(
        "[ERROR] Manufacturer ID is not Texas Instruments (0x5449).");
    return false;
  }

  uint16_t deviceNumber = deviceId >> 4;
  uint8_t revision = deviceId & 0x0F;

  if (deviceNumber != 0x228) {
    Serial.print("[ERROR] Device number is 0x");
    Serial.print(deviceNumber, HEX);
    Serial.println(", expected INA228 device number 0x228.");
    return false;
  }

  Serial.print("[INA228] Revision        = ");
  Serial.println(revision);

  Serial.println("[INA228] Identity check: PASS");
  return true;
}

bool configureIna228() {
  Serial.println("[INA228] Configuring sensor...");

  // CONFIG = 0 keeps ADCRANGE = 0.
  //
  // ADCRANGE = 0 is important because our existing shunt-voltage calibration
  // assumes the INA228's 312.5 nV/count VSHUNT scale.
  if (!writeRegister16(REG_CONFIG, 0x0000)) {
    return false;
  }

  if (!writeRegister16(REG_ADC_CONFIG, ADC_CONFIG_VALUE)) {
    return false;
  }

  if (!writeRegister16(REG_SHUNT_CAL, SHUNT_CAL_VALUE)) {
    return false;
  }

  // Read every important configuration value back.
  //
  // A successful I2C write only means the transmission succeeded.
  // We also want proof that the INA228 is actually holding the values we asked
  // it to hold.
  uint16_t configReadback;
  uint16_t adcReadback;
  uint16_t shuntCalReadback;

  if (!readRegister16(REG_CONFIG, configReadback) ||
      !readRegister16(REG_ADC_CONFIG, adcReadback) ||
      !readRegister16(REG_SHUNT_CAL, shuntCalReadback)) {
    Serial.println("[ERROR] Could not read INA228 configuration back.");
    return false;
  }

  if ((configReadback & 0x0010) != 0) {
    Serial.println(
        "[ERROR] INA228 ADCRANGE is not 0. Calibration would be wrong.");
    return false;
  }

  if (adcReadback != ADC_CONFIG_VALUE) {
    Serial.print("[ERROR] ADC_CONFIG readback mismatch. Got 0x");
    Serial.print(adcReadback, HEX);
    Serial.print(", expected 0x");
    Serial.println(ADC_CONFIG_VALUE, HEX);
    return false;
  }

  if (shuntCalReadback != SHUNT_CAL_VALUE) {
    Serial.print("[ERROR] SHUNT_CAL readback mismatch. Got ");
    Serial.print(shuntCalReadback);
    Serial.print(", expected ");
    Serial.println(SHUNT_CAL_VALUE);
    return false;
  }

  Serial.println(
      "[INA228] CONFIG / ADC_CONFIG / SHUNT_CAL readback: PASS");
  Serial.println("[INA228] Configuration complete.");

  return true;
}

bool resetInaAccumulators() {
  Serial.println(
      "[INA228] Resetting ENERGY and CHARGE accumulators...");

  uint16_t config;

  if (!readRegister16(REG_CONFIG, config)) {
    Serial.println(
        "[ERROR] Cannot reset accumulators because CONFIG read failed.");
    return false;
  }

  // CONFIG bit 14 is RSTACC.
  //
  // Writing a 1 to this bit resets both the ENERGY and CHARGE hardware
  // accumulators. The bit clears itself automatically afterward.
  if (!writeRegister16(REG_CONFIG, config | 0x4000)) {
    Serial.println("[ERROR] Accumulator reset command failed.");
    return false;
  }

  delay(2);

  // Read the accumulators back immediately so that a failed reset cannot pass
  // unnoticed.
  uint64_t energyRaw;
  int64_t chargeRaw;

  if (!readRegister40Unsigned(REG_ENERGY, energyRaw) ||
      !readRegister40Signed(REG_CHARGE, chargeRaw)) {
    Serial.println("[ERROR] Could not verify accumulator reset.");
    return false;
  }

  if (energyRaw != 0 || chargeRaw != 0) {
    Serial.println(
        "[WARNING] Accumulator reset completed, but immediate readback was "
        "not zero.");
  } else {
    Serial.println("[INA228] Accumulators reset: OK");
  }

  return true;
}

bool readSensor(SensorReading &reading, bool verboseVshunt) {
  uint32_t rawBus;
  int32_t rawCurrent;
  uint32_t rawPower;
  uint16_t rawTempUnsigned;
  int32_t rawVshunt;

  if (!readRegister20Unsigned(REG_VBUS, rawBus) ||
      !readRegister20Signed(REG_CURRENT, rawCurrent) ||
      !readRegister24Unsigned(REG_POWER, rawPower) ||
      !readRegister16(REG_DIETEMP, rawTempUnsigned) ||
      !readRegister20Signed(REG_VSHUNT, rawVshunt)) {
    Serial.println("[ERROR] Sensor measurement read failed.");
    return false;
  }

  int16_t rawTemp = static_cast<int16_t>(rawTempUnsigned);

  reading.voltage_V =
      rawBus * VBUS_LSB_V;

  reading.current_mA =
      rawCurrent * CURRENT_LSB_A * 1000.0;

  reading.power_mW =
      rawPower * POWER_LSB_W * 1000.0;

  reading.temperature_C =
      rawTemp * DIETEMP_LSB_C;

  reading.rawVshuntCounts =
      rawVshunt;

  reading.shuntVoltage_mV =
      rawVshunt * VSHUNT_LSB_V * 1000.0;

  reading.currentFromShunt_mA =
      (reading.shuntVoltage_mV / 1000.0) /
      SHUNT_OHMS *
      1000.0;

  if (verboseVshunt) {
    Serial.println("[VSHUNT] Reading raw shunt ADC...");

    Serial.print("[VSHUNT] Signed ADC counts    = ");
    Serial.println(reading.rawVshuntCounts);

    Serial.print("[VSHUNT] Shunt voltage        = ");
    Serial.print(reading.shuntVoltage_mV, 6);
    Serial.println(" mV");

    Serial.print("[VSHUNT] Assumed shunt R      = ");
    Serial.print(SHUNT_OHMS * 1000.0, 4);
    Serial.println(" mOhm");

    Serial.print("[VSHUNT] V/R current          = ");
    Serial.print(reading.currentFromShunt_mA, 3);
    Serial.println(" mA");

    Serial.print("[CURRENT] INA CURRENT register = ");
    Serial.print(reading.current_mA, 3);
    Serial.println(" mA");

    Serial.print("[COMPARE] Difference           = ");
    Serial.print(
        reading.current_mA -
        reading.currentFromShunt_mA,
        3);
    Serial.println(" mA");
  }

  return true;
}

bool readAccumulatedCharge_mAh(double &charge_mAh) {
  int64_t rawCharge;

  if (!readRegister40Signed(REG_CHARGE, rawCharge)) {
    return false;
  }

  // CHARGE register units are CURRENT_LSB coulombs/count.
  //
  // 1 mAh = 3.6 coulombs.
  charge_mAh =
      rawCharge *
      CHARGE_LSB_C /
      3.6;

  return true;
}

bool readAccumulatedEnergy_mWh(double &energy_mWh) {
  uint64_t rawEnergy;

  if (!readRegister40Unsigned(REG_ENERGY, rawEnergy)) {
    return false;
  }

  // ENERGY register is stored in joules.
  //
  // 1 mWh = 3.6 joules.
  energy_mWh =
      rawEnergy *
      ENERGY_LSB_J /
      3.6;

  return true;
}

// -----------------------------------------------------------------------------
// NVS persistence
//
// NVS is nonvolatile flash storage inside the ESP32.
//
// That means the running totals can survive a complete power loss.
//
// We currently write a checkpoint every five completed intervals so that we do
// not write flash every single minute.
// -----------------------------------------------------------------------------

bool saveCheckpoint() {
  Serial.println();

  Serial.print("[NVS] Writing checkpoint for interval ");
  Serial.println(completedInterval);

  if (!preferences.begin(NVS_NAMESPACE, false)) {
    Serial.println(
        "[ERROR] Could not open NVS namespace for writing.");
    return false;
  }

  bool ok = true;

  Serial.print("[NVS] Writing schema = ");
  Serial.println(NVS_SCHEMA_VERSION);

  if (preferences.putUInt(
          "schema",
          NVS_SCHEMA_VERSION) != sizeof(uint32_t)) {
    Serial.println("[ERROR] Failed to write NVS key: schema");
    ok = false;
  }

  Serial.print("[NVS] Writing interval = ");
  Serial.println(completedInterval);

  if (preferences.putUInt(
          "interval",
          completedInterval) != sizeof(uint32_t)) {
    Serial.println("[ERROR] Failed to write NVS key: interval");
    ok = false;
  }

  Serial.print("[NVS] Writing charge = ");
  Serial.print(runningCharge_mAh, 6);
  Serial.println(" mAh");

  if (preferences.putDouble(
          "charge",
          runningCharge_mAh) != sizeof(double)) {
    Serial.println("[ERROR] Failed to write NVS key: charge");
    ok = false;
  }

  Serial.print("[NVS] Writing energy = ");
  Serial.print(runningEnergy_mWh, 6);
  Serial.println(" mWh");

  if (preferences.putDouble(
          "energy",
          runningEnergy_mWh) != sizeof(double)) {
    Serial.println("[ERROR] Failed to write NVS key: energy");
    ok = false;
  }

  preferences.end();

  if (ok) {
    Serial.println(
        "*** NVS CHECKPOINT SAVED SUCCESSFULLY ***");
  } else {
    Serial.println(
        "*** ERROR: NVS CHECKPOINT WAS NOT FULLY SAVED ***");
  }

  return ok;
}

bool loadCheckpoint() {
  Serial.println();
  Serial.println("[NVS] Beginning checkpoint load...");

  Serial.print("[NVS] Opening namespace \"");
  Serial.print(NVS_NAMESPACE);
  Serial.println("\" in READ ONLY mode...");

  if (!preferences.begin(NVS_NAMESPACE, true)) {
    Serial.println(
        "[WARNING] NVS namespace does not exist yet. Starting at zero.");

    completedInterval = 0;
    runningCharge_mAh = 0.0;
    runningEnergy_mWh = 0.0;

    return true;
  }

  Serial.println("[NVS] Namespace opened.");

  if (!preferences.isKey("schema") ||
      !preferences.isKey("interval") ||
      !preferences.isKey("charge") ||
      !preferences.isKey("energy")) {
    Serial.println(
        "[WARNING] NVS checkpoint is incomplete. Starting at zero.");

    preferences.end();

    completedInterval = 0;
    runningCharge_mAh = 0.0;
    runningEnergy_mWh = 0.0;

    return true;
  }

  uint32_t schema =
      preferences.getUInt("schema", 0);

  Serial.print("[NVS] Stored schema = ");
  Serial.println(schema);

  if (schema != NVS_SCHEMA_VERSION) {
    Serial.print("[ERROR] Unsupported NVS schema. Expected ");
    Serial.print(NVS_SCHEMA_VERSION);
    Serial.print(", found ");
    Serial.println(schema);

    preferences.end();
    return false;
  }

  completedInterval =
      preferences.getUInt("interval", 0);

  runningCharge_mAh =
      preferences.getDouble("charge", 0.0);

  runningEnergy_mWh =
      preferences.getDouble("energy", 0.0);

  Serial.print("[NVS] interval = ");
  Serial.println(completedInterval);

  Serial.print("[NVS] charge = ");
  Serial.print(runningCharge_mAh, 6);
  Serial.println(" mAh");

  Serial.print("[NVS] energy = ");
  Serial.print(runningEnergy_mWh, 6);
  Serial.println(" mWh");

  preferences.end();

  Serial.println("[NVS] Namespace closed.");
  Serial.println("[NVS] Checkpoint load: OK");

  return true;
}

// -----------------------------------------------------------------------------
// CSV output
//
// Human-readable diagnostics remain intact.
//
// These lines exist specifically so we can save Serial output and extract a
// clean dataset later.
// -----------------------------------------------------------------------------

void printCsvHeader() {
  Serial.println();

  Serial.println(
      "[CSV] Machine-readable interval logging enabled.");

  Serial.println(
      "CSV_HEADER,"
      "interval,"
      "elapsed_seconds,"
      "voltage_V,"
      "current_mA,"
      "power_mW,"
      "temperature_C,"
      "interval_charge_mAh,"
      "interval_energy_mWh,"
      "average_current_mA,"
      "average_power_mW,"
      "running_charge_mAh,"
      "running_energy_mWh");
}

void printCsvRow(
    uint32_t interval,
    double elapsedSeconds,
    const SensorReading &reading,
    double intervalCharge_mAh,
    double intervalEnergy_mWh,
    double averageCurrent_mA,
    double averagePower_mW) {

  Serial.print("CSV_DATA,");
  Serial.print(interval);

  Serial.print(',');
  Serial.print(elapsedSeconds, 3);

  Serial.print(',');
  Serial.print(reading.voltage_V, 6);

  Serial.print(',');
  Serial.print(reading.current_mA, 6);

  Serial.print(',');
  Serial.print(reading.power_mW, 6);

  Serial.print(',');
  Serial.print(reading.temperature_C, 4);

  Serial.print(',');
  Serial.print(intervalCharge_mAh, 9);

  Serial.print(',');
  Serial.print(intervalEnergy_mWh, 9);

  Serial.print(',');
  Serial.print(averageCurrent_mA, 6);

  Serial.print(',');
  Serial.print(averagePower_mW, 6);

  Serial.print(',');
  Serial.print(runningCharge_mAh, 9);

  Serial.print(',');
  Serial.println(runningEnergy_mWh, 9);
}

// -----------------------------------------------------------------------------
// Serial command interface
//
// Commands are deliberately simple text commands.
//
// RESET requires the explicit phrase RESET YES so that an accidental newline or
// typo cannot erase our experiment totals.
// -----------------------------------------------------------------------------

void printHelp() {
  Serial.println();

  Serial.println("[COMMAND] Available commands:");

  Serial.println(
      "  HELP       - show this list");

  Serial.println(
      "  STATUS     - print current logger totals and live sensor reading");

  Serial.println(
      "  RESET YES  - ZERO interval, charge, and energy totals; save zeroes "
      "to NVS;");

  Serial.println(
      "               reset INA228 accumulators; start a fresh 60-second "
      "interval");

  Serial.println();
}

void printStatus() {
  Serial.println();

  Serial.println("[STATUS] Current logger state:");

  Serial.print("[STATUS] Completed interval = ");
  Serial.println(completedInterval);

  Serial.print("[STATUS] Running charge     = ");
  Serial.print(runningCharge_mAh, 6);
  Serial.println(" mAh");

  Serial.print("[STATUS] Running energy     = ");
  Serial.print(runningEnergy_mWh, 6);
  Serial.println(" mWh");

  SensorReading reading;

  if (readSensor(reading, false)) {
    Serial.print("[STATUS] Voltage            = ");
    Serial.print(reading.voltage_V, 4);
    Serial.println(" V");

    Serial.print("[STATUS] Current            = ");
    Serial.print(reading.current_mA, 3);
    Serial.println(" mA");

    Serial.print("[STATUS] Power              = ");
    Serial.print(reading.power_mW, 3);
    Serial.println(" mW");
  } else {
    Serial.println(
        "[ERROR] STATUS could not read INA228.");
  }
}

bool resetExperiment() {
  Serial.println();

  Serial.println(
      "============================================================");

  Serial.println("[RESET] RESET YES received.");
  Serial.println("[RESET] Starting a NEW experiment from zero.");
  Serial.println("[RESET] Old running totals will be discarded.");

  if (!resetInaAccumulators()) {
    Serial.println(
        "[ERROR] Experiment reset aborted because INA228 reset failed.");

    Serial.println(
        "============================================================");

    return false;
  }

  completedInterval = 0;
  runningCharge_mAh = 0.0;
  runningEnergy_mWh = 0.0;

  if (!saveCheckpoint()) {
    Serial.println(
        "[ERROR] Experiment reset aborted because zero checkpoint failed.");

    Serial.println(
        "============================================================");

    return false;
  }

  intervalStartMs = millis();
  lastHeartbeatMs = intervalStartMs;

  Serial.println("[RESET] interval = 0");
  Serial.println("[RESET] running charge = 0.000000 mAh");
  Serial.println("[RESET] running energy = 0.000000 mWh");
  Serial.println("[RESET] New 60-second interval started NOW.");
  Serial.println("[RESET] RESET COMPLETE: ALL OK");

  Serial.println(
      "============================================================");

  printCsvHeader();

  return true;
}

void processCommand(String command) {
  command.trim();
  command.toUpperCase();

  if (command.length() == 0) {
    return;
  }

  Serial.print("[COMMAND] Received: ");
  Serial.println(command);

  if (command == "HELP") {
    printHelp();
  } else if (command == "STATUS") {
    printStatus();
  } else if (command == "RESET") {
    Serial.println(
        "[COMMAND] RESET requires confirmation. Type exactly: RESET YES");
  } else if (command == "RESET YES") {
    resetExperiment();
  } else {
    Serial.print("[WARNING] Unknown command: ");
    Serial.println(command);

    Serial.println(
        "[WARNING] Type HELP for available commands.");
  }
}

void handleSerialCommands() {
  while (Serial.available()) {
    char c =
        static_cast<char>(Serial.read());

    if (c == '\n' || c == '\r') {
      if (serialCommandBuffer.length() > 0) {
        processCommand(serialCommandBuffer);
        serialCommandBuffer = "";
      }

      continue;
    }

    if (serialCommandBuffer.length() >= 80) {
      Serial.println(
          "[ERROR] Serial command exceeded 80 characters. Buffer cleared.");

      serialCommandBuffer = "";
      continue;
    }

    serialCommandBuffer += c;
  }
}

// -----------------------------------------------------------------------------
// Interval and heartbeat output
// -----------------------------------------------------------------------------

void printHeartbeat() {
  SensorReading reading;

  if (!readSensor(reading, false)) {
    Serial.println(
        "[HEARTBEAT] ERROR: INA228 read failed");
    return;
  }

  unsigned long elapsedMs =
      millis() - intervalStartMs;

  Serial.print("[HEARTBEAT] ALL OK | next interval ");
  Serial.print(completedInterval + 1);

  Serial.print(" | elapsed ");
  Serial.print(elapsedMs / 1000.0, 1);
  Serial.print(" s | ");

  Serial.print(reading.voltage_V, 4);
  Serial.print(" V | ");

  Serial.print(reading.current_mA, 3);
  Serial.print(" mA | ");

  Serial.print(reading.power_mW, 3);
  Serial.println(" mW");
}

bool closeMeasurementInterval() {
  unsigned long endMs =
      millis();

  unsigned long elapsedMs =
      endMs - intervalStartMs;

  double elapsedSeconds =
      elapsedMs / 1000.0;

  Serial.println();

  Serial.println(
      "============================================================");

  Serial.println("[LOGGER] INTERVAL TIMER EXPIRED");

  Serial.print("[LOGGER] Actual elapsed time = ");
  Serial.print(elapsedSeconds, 3);
  Serial.println(" seconds");

  Serial.println();

  Serial.println(
      "[INA228] Capturing end-of-interval measurements...");

  SensorReading reading;

  if (!readSensor(reading, false)) {
    Serial.println(
        "[ERROR] Could not close interval because end measurement failed.");

    return false;
  }

  Serial.println(
      "[INA228] Reading accumulated CHARGE...");

  double intervalCharge_mAh;

  if (!readAccumulatedCharge_mAh(intervalCharge_mAh)) {
    Serial.println(
        "[ERROR] Could not read accumulated CHARGE.");

    return false;
  }

  Serial.println(
      "[INA228] Reading accumulated ENERGY...");

  double intervalEnergy_mWh;

  if (!readAccumulatedEnergy_mWh(intervalEnergy_mWh)) {
    Serial.println(
        "[ERROR] Could not read accumulated ENERGY.");

    return false;
  }

  // Capture the completed interval BEFORE resetting the hardware accumulators.
  // After this point, the values above are our permanent record of this minute.
  Serial.println(
      "[INA228] Closing completed interval.");

  if (!resetInaAccumulators()) {
    Serial.println(
        "[ERROR] New interval was NOT started because accumulator reset failed.");

    return false;
  }

  intervalStartMs = millis();

  Serial.println(
      "[INA228] New accumulation interval started.");

  completedInterval++;

  runningCharge_mAh +=
      intervalCharge_mAh;

  runningEnergy_mWh +=
      intervalEnergy_mWh;

  double averageCurrent_mA =
      intervalCharge_mAh *
      3600.0 /
      elapsedSeconds;

  double averagePower_mW =
      intervalEnergy_mWh *
      3600.0 /
      elapsedSeconds;

  // Read VSHUNT once more for the diagnostic block.
  //
  // It may differ by a few ADC counts from the end reading because a little
  // time has passed. That is expected.
  SensorReading diagnosticReading;

  if (!readSensor(diagnosticReading, true)) {
    Serial.println(
        "[ERROR] Interval data was captured, but VSHUNT diagnostics failed.");

    diagnosticReading = reading;
  }

  double shuntPower_mW =
      (diagnosticReading.shuntVoltage_mV / 1000.0) *
      (diagnosticReading.current_mA / 1000.0) *
      1000.0;

  double shuntRangePercent =
      fabs(
          diagnosticReading.shuntVoltage_mV /
          1000.0) /
      VSHUNT_FULL_SCALE_V *
      100.0;

  Serial.println();

  Serial.println(
      "------------------------------------------------------------");

  Serial.print("INTERVAL #");
  Serial.println(completedInterval);

  Serial.print("Actual interval:     ");
  Serial.print(elapsedSeconds, 3);
  Serial.println(" seconds");

  Serial.println();

  Serial.print("Ending voltage:      ");
  Serial.print(reading.voltage_V, 4);
  Serial.println(" V");

  Serial.print("Current right now:   ");
  Serial.print(reading.current_mA, 3);
  Serial.println(" mA");

  Serial.print("Power right now:     ");
  Serial.print(reading.power_mW, 3);
  Serial.println(" mW");

  Serial.print("Temperature:         ");
  Serial.print(reading.temperature_C, 2);
  Serial.println(" C");

  Serial.println();

  Serial.print("Raw VSHUNT counts:   ");
  Serial.println(diagnosticReading.rawVshuntCounts);

  Serial.print("Shunt voltage:       ");
  Serial.print(
      diagnosticReading.shuntVoltage_mV,
      6);
  Serial.println(" mV");

  Serial.print("VSHUNT/R current:    ");
  Serial.print(
      diagnosticReading.currentFromShunt_mA,
      3);
  Serial.println(" mA");

  Serial.print("CURRENT register:    ");
  Serial.print(
      diagnosticReading.current_mA,
      3);
  Serial.println(" mA");

  Serial.print("Current difference:  ");
  Serial.print(
      diagnosticReading.current_mA -
      diagnosticReading.currentFromShunt_mA,
      3);
  Serial.println(" mA");

  Serial.println();

  Serial.print("Interval charge:     ");
  Serial.print(intervalCharge_mAh, 6);
  Serial.println(" mAh");

  Serial.print("Interval energy:     ");
  Serial.print(intervalEnergy_mWh, 6);
  Serial.println(" mWh");

  Serial.print("Average current:     ");
  Serial.print(averageCurrent_mA, 3);
  Serial.println(" mA");

  Serial.print("Average power:       ");
  Serial.print(averagePower_mW, 3);
  Serial.println(" mW");

  Serial.println();

  Serial.print("RUNNING charge:      ");
  Serial.print(runningCharge_mAh, 6);
  Serial.println(" mAh");

  Serial.print("RUNNING energy:      ");
  Serial.print(runningEnergy_mWh, 6);
  Serial.println(" mWh");

  Serial.print("Shunt power:         ");
  Serial.print(shuntPower_mW, 3);
  Serial.println(" mW");

  Serial.print("Shunt range used:    ");
  Serial.print(shuntRangePercent, 3);
  Serial.println(" %");

  Serial.println(
      "------------------------------------------------------------");

  // One compact machine-readable row for graphing.
  //
  // We keep the verbose block above because it is easier for a human to debug.
  // This CSV_DATA line is easier for software to parse.
  printCsvRow(
      completedInterval,
      elapsedSeconds,
      reading,
      intervalCharge_mAh,
      intervalEnergy_mWh,
      averageCurrent_mA,
      averagePower_mW);

  if (
      completedInterval %
      NVS_CHECKPOINT_EVERY_INTERVALS ==
      0) {

    Serial.println(
        "[NVS] CHECKPOINT REQUIRED");

    if (!saveCheckpoint()) {
      Serial.println(
          "[ERROR] Measurement succeeded, but NVS checkpoint failed.");
    }

  } else {

    uint32_t remaining =
        NVS_CHECKPOINT_EVERY_INTERVALS -
        (completedInterval %
         NVS_CHECKPOINT_EVERY_INTERVALS);

    Serial.print(
        "[NVS] No checkpoint required. Next checkpoint in ");

    Serial.print(remaining);
    Serial.println(" interval(s).");
  }

  return true;
}

// -----------------------------------------------------------------------------
// Arduino entry points
// -----------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();

  Serial.println(
      "============================================================");

  Serial.println(
      "BMW SOLAR LOGGER - DEVELOPMENT FIRMWARE");

  Serial.println(
      "INA228 + XIAO ESP32-C3");

  Serial.println(
      "============================================================");

  Serial.println();

  Serial.println("[BOOT] Starting I2C...");

  // XIAO ESP32-C3:
  //
  // D4 -> SDA
  // D5 -> SCL
  Wire.begin(D4, D5);

  Wire.setClock(400000);

  Serial.println(
      "[BOOT] I2C started: SDA=D4, SCL=D5, 400 kHz");

  if (!verifyInaIdentity()) {
    Serial.println(
        "[FATAL] INA228 identity check failed. Logger halted.");

    while (true) {
      Serial.println(
          "[FATAL] NOT ALL OK - fix INA228/I2C wiring and reset.");

      delay(5000);
    }
  }

  if (!configureIna228()) {
    Serial.println(
        "[FATAL] INA228 configuration failed. Logger halted.");

    while (true) {
      Serial.println(
          "[FATAL] NOT ALL OK - configuration failure.");

      delay(5000);
    }
  }

  Serial.println();

  Serial.println(
      "[BOOT] Loading persistent state from NVS...");

  if (!loadCheckpoint()) {
    Serial.println(
        "[FATAL] NVS load failed. Logger halted to avoid corrupting totals.");

    while (true) {
      Serial.println(
          "[FATAL] NOT ALL OK - NVS state requires attention.");

      delay(5000);
    }
  }

  Serial.println();

  Serial.println(
      "[INA228] Waiting 2 seconds for ADC measurements to settle...");

  delay(2000);

  Serial.println(
      "[INA228] ADC settle complete.");

  Serial.println();

  Serial.println(
      "[INA228] Reading current sensor state...");

  SensorReading reading;

  if (!readSensor(reading, true)) {
    Serial.println(
        "[FATAL] Initial INA228 measurement failed. Logger halted.");

    while (true) {
      Serial.println(
          "[FATAL] NOT ALL OK - initial measurement failure.");

      delay(5000);
    }
  }

  Serial.print("[INA228] Voltage      = ");
  Serial.print(reading.voltage_V, 4);
  Serial.println(" V");

  Serial.print("[INA228] Current      = ");
  Serial.print(reading.current_mA, 3);
  Serial.println(" mA");

  Serial.print("[INA228] Power        = ");
  Serial.print(reading.power_mW, 3);
  Serial.println(" mW");

  Serial.print("[INA228] Temperature  = ");
  Serial.print(reading.temperature_C, 2);
  Serial.println(" C");

  Serial.println();

  Serial.println(
      "[LOGGER] Preparing measurement interval...");

  if (!resetInaAccumulators()) {
    Serial.println(
        "[FATAL] Could not initialize accumulation interval. Logger halted.");

    while (true) {
      Serial.println(
          "[FATAL] NOT ALL OK - accumulator reset failure.");

      delay(5000);
    }
  }

  intervalStartMs = millis();
  lastHeartbeatMs = intervalStartMs;

  Serial.println(
      "[LOGGER] Measurement interval started.");

  Serial.println();

  Serial.println(
      "============================================================");

  Serial.println("ALL OK");
  Serial.println(
      "Initialization completed successfully.");

  Serial.println();

  Serial.println(
      "Measurement interval: 60 seconds");

  Serial.println(
      "Heartbeat interval:   5 seconds");

  Serial.print(
      "NVS checkpoint every: ");

  Serial.print(
      NVS_CHECKPOINT_EVERY_INTERVALS);

  Serial.println(
      " intervals");

  Serial.print(
      "Shunt resistance:     ");

  Serial.print(
      SHUNT_OHMS * 1000.0,
      4);

  Serial.println(
      " mOhm");

  Serial.print(
      "VSHUNT LSB:           ");

  Serial.print(
      VSHUNT_LSB_V * 1.0e9,
      1);

  Serial.println(
      " nV/count");

  Serial.println();

  Serial.print(
      "Restored interval:    ");

  Serial.println(
      completedInterval);

  Serial.print(
      "Restored charge:      ");

  Serial.print(
      runningCharge_mAh,
      6);

  Serial.println(
      " mAh");

  Serial.print(
      "Restored energy:      ");

  Serial.print(
      runningEnergy_mWh,
      6);

  Serial.println(
      " mWh");

  Serial.println(
      "============================================================");

  printCsvHeader();
  printHelp();
}

void loop() {
  // Check for commands such as STATUS and RESET YES.
  handleSerialCommands();

  unsigned long now =
      millis();

  if (
      now - lastHeartbeatMs >=
      HEARTBEAT_INTERVAL_MS) {

    lastHeartbeatMs = now;
    printHeartbeat();
  }

  if (
      now - intervalStartMs >=
      MEASUREMENT_INTERVAL_MS) {

    if (!closeMeasurementInterval()) {
      Serial.println(
          "[ERROR] Interval close failed. Keeping logger awake for diagnosis.");

      // Do not silently restart the interval timer here.
      //
      // If something fails, we want the failure to remain obvious instead of
      // pretending that another clean interval started.
      delay(1000);
    }
  }
}