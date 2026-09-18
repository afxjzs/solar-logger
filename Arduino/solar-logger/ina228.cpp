// ============================================================================
// INA228 DRIVER - implementation
// ============================================================================
//
// Moved out of solar-logger.ino unchanged, apart from `static` on the helpers
// that nothing outside this file calls. See ina228.h for what the driver owns
// and what it deliberately leaves to its callers.
//
// A .cpp is compiled on its own, without the #include <Arduino.h> that Arduino
// CLI prepends to the sketch, so both includes below are required.
// ============================================================================

#include "ina228.h"

#include <Arduino.h>
#include <Wire.h>

// ----------------------------------------------------------------------------
// Private register map and scale factors. ina228.h carries the register
// overview and every constant that code outside the driver uses.
// ----------------------------------------------------------------------------

constexpr uint8_t INA228_ADDRESS = 0x40;

constexpr uint8_t REG_VSHUNT = 0x04;
constexpr uint8_t REG_VBUS = 0x05;
constexpr uint8_t REG_DIETEMP = 0x06;
constexpr uint8_t REG_CURRENT = 0x07;
constexpr uint8_t REG_POWER = 0x08;
constexpr uint8_t REG_ENERGY = 0x09;
constexpr uint8_t REG_CHARGE = 0x0A;

// ADC_CONFIG MODE field layout and shutdown encodings. The datasheet table and
// the decoding of ADC_CONFIG_VALUE are in ina228.h.
constexpr uint16_t INA_MODE_MASK = 0xF000;
constexpr uint8_t INA_MODE_SHIFT = 12;

constexpr uint16_t INA_MODE_SHUTDOWN_ZERO = 0x0;
constexpr uint16_t INA_MODE_SHUTDOWN_EIGHT = 0x8;

// Datasheet Section 7.4.1 and the Electrical Characteristics table:
//
//   IQ    (active)    640 uA typical, 750 uA maximum, VSENSE = 0 V
//   IQSD  (shutdown)  2.8 uA typical, 5 uA maximum
//   TPOR  from shutdown mode   60 us device start-up time
//
// So the arithmetic prediction for what INA228 shutdown saves is roughly
// 640 uA. The existing 2-second ADC settle in setup() is four orders of
// magnitude longer than the 60 us start-up time, so no new settling delay is
// needed on the wake path.

constexpr double CURRENT_LSB_A = 4.0e-6;
constexpr double VBUS_LSB_V = 195.3125e-6;
constexpr double DIETEMP_LSB_C = 0.0078125;

// The INA228 POWER and ENERGY register scales are derived from CURRENT_LSB.
//
// POWER_LSB:
//   each POWER-register count represents this many watts.
//
// ENERGY_LSB:
//   each ENERGY-register count represents this many joules.
//
// CHARGE_LSB:
//   each CHARGE-register count represents this many coulombs.
constexpr double POWER_LSB_W = 3.2 * CURRENT_LSB_A;
constexpr double ENERGY_LSB_J = 16.0 * POWER_LSB_W;
constexpr double CHARGE_LSB_C = CURRENT_LSB_A;

// ============================================================================
// SIGN EXTENSION HELPERS
// ============================================================================
//
// Computers need to know whether a binary number is signed.
//
// The INA228 has some signed 20-bit and 40-bit values.
//
// C++ normally works with convenient types such as:
//   int32_t = 32 bits
//   int64_t = 64 bits
//
// Therefore we need to convert the INA228's smaller signed binary number into
// the equivalent normal C++ signed integer.
//
// This is called SIGN EXTENSION.
//
// You do not need to memorize the bit manipulation here. Conceptually:
//
//   INA gives us a signed 20-bit number
//                |
//                v
//   signExtend20()
//                |
//                v
//   normal signed 32-bit C++ integer
// ============================================================================

static int32_t signExtend20(uint32_t value)
{
	value &= 0xFFFFF;

	// Bit 19 is the sign bit of a 20-bit signed number.
	if (value & 0x80000)
	{
		value |= 0xFFF00000;
	}

	return static_cast<int32_t>(value);
}

static int64_t signExtend40(uint64_t value)
{
	value &= 0xFFFFFFFFFFULL;

	// Bit 39 is the sign bit of a 40-bit signed number.
	if (value & 0x8000000000ULL)
	{
		value |= 0xFFFFFF0000000000ULL;
	}

	return static_cast<int64_t>(value);
}

// ============================================================================
// LOW-LEVEL I2C REGISTER READING
// ============================================================================
//
// This is one of the more firmware-specific parts of the program.
//
// Suppose we want to read VBUS.
//
// VBUS lives in INA228 register 0x05.
//
// The conversation over I2C is approximately:
//
//   ESP32 -> INA228:
//       "I want register 0x05."
//
//   ESP32 -> INA228:
//       "Now send me 3 bytes."
//
//   INA228 -> ESP32:
//       byte 1
//       byte 2
//       byte 3
//
// Those three bytes are then combined into one numeric value.
//
// IMPORTANT:
//
// The "false" passed to:
//
//   Wire.endTransmission(false)
//
// tells the ESP32 NOT to fully release the I2C transaction yet.
//
// This creates what I2C calls a "repeated start":
//
//   START
//   write register address
//   REPEATED START
//   read register data
//   STOP
//
// Many register-based I2C devices expect this pattern.
// ============================================================================

static bool readRegisterBytes(uint8_t reg, uint8_t *buffer, size_t length)
{
	// Begin addressing the INA228.
	Wire.beginTransmission(INA228_ADDRESS);

	// Send ONE byte containing the register address we want to read.
	Wire.write(reg);

	// false = keep the transaction active for the read that follows.
	uint8_t txResult = Wire.endTransmission(false);

	if (txResult != 0)
	{
		Serial.print("[ERROR] I2C register-address phase failed for register 0x");
		Serial.print(reg, HEX);
		Serial.print(". Wire error = ");
		Serial.println(txResult);
		return false;
	}

	// Ask the INA228 to return exactly "length" bytes.
	size_t received =
			Wire.requestFrom(
					INA228_ADDRESS,
					static_cast<uint8_t>(length));

	// If we asked for 3 bytes and received only 2, that is a real error.
	//
	// We do NOT silently turn a failed read into a zero measurement.
	if (received != length)
	{
		Serial.print("[ERROR] I2C short read for register 0x");
		Serial.print(reg, HEX);
		Serial.print(". Expected ");
		Serial.print(length);
		Serial.print(" byte(s), received ");
		Serial.println(received);
		return false;
	}

	// Pull each received byte out of Arduino's internal I2C receive buffer and
	// place it into the array supplied by the caller.
	for (size_t i = 0; i < length; i++)
	{
		if (!Wire.available())
		{
			Serial.print("[ERROR] I2C buffer unexpectedly empty at register 0x");
			Serial.println(reg, HEX);
			return false;
		}

		buffer[i] = Wire.read();
	}

	return true;
}

// ============================================================================
// READING A 16-BIT REGISTER
// ============================================================================
//
// If INA228 sends:
//
//   byte[0] = 0x54
//   byte[1] = 0x49
//
// we need to construct:
//
//   0x5449
//
// "<< 8" means:
//
//   move the first byte left by 8 binary positions
//
// so:
//
//   0x54 becomes 0x5400
//
// then:
//
//   0x5400 | 0x49
//
// becomes:
//
//   0x5449
//
// "|" is bitwise OR. Here it effectively combines the two bytes.
// ============================================================================

bool readRegister16(uint8_t reg, uint16_t &value)
{
	uint8_t bytes[2];

	if (!readRegisterBytes(reg, bytes, sizeof(bytes)))
	{
		return false;
	}

	value =
			(static_cast<uint16_t>(bytes[0]) << 8) |
			bytes[1];

	return true;
}

// ============================================================================
// WRITING A 16-BIT REGISTER
// ============================================================================
//
// This is the opposite operation.
//
// Given:
//
//   value = 0xFB6B
//
// we transmit:
//
//   register address
//   0xFB
//   0x6B
// ============================================================================

static bool writeRegister16(uint8_t reg, uint16_t value)
{
	Wire.beginTransmission(INA228_ADDRESS);

	Wire.write(reg);

	// Upper byte.
	Wire.write(static_cast<uint8_t>(value >> 8));

	// Lower byte.
	Wire.write(static_cast<uint8_t>(value & 0xFF));

	uint8_t result = Wire.endTransmission();

	if (result != 0)
	{
		Serial.print("[ERROR] I2C write failed for register 0x");
		Serial.print(reg, HEX);
		Serial.print(". Wire error = ");
		Serial.println(result);
		return false;
	}

	return true;
}

// ============================================================================
// READING INA228 20-BIT MEASUREMENT REGISTERS
// ============================================================================
//
// Several INA228 measurements are 20-bit values.
//
// Physically, I2C still transfers complete bytes, so the INA sends 24 bits:
//
//   byte 1
//   byte 2
//   byte 3
//
// The useful 20-bit value occupies the upper 20 bits.
//
// Therefore:
//
//   raw24 >> 4
//
// discards the four unused low bits.
//
// CURRENT and VSHUNT are SIGNED because current can flow either direction.
//
// VBUS is UNSIGNED because bus voltage is not represented as negative.
// ============================================================================

static bool readRegister20Signed(uint8_t reg, int32_t &value)
{
	uint8_t bytes[3];

	if (!readRegisterBytes(reg, bytes, sizeof(bytes)))
	{
		return false;
	}

	uint32_t raw24 =
			(static_cast<uint32_t>(bytes[0]) << 16) |
			(static_cast<uint32_t>(bytes[1]) << 8) |
			bytes[2];

	uint32_t raw20 = raw24 >> 4;

	value = signExtend20(raw20);

	return true;
}

static bool readRegister20Unsigned(uint8_t reg, uint32_t &value)
{
	uint8_t bytes[3];

	if (!readRegisterBytes(reg, bytes, sizeof(bytes)))
	{
		return false;
	}

	uint32_t raw24 =
			(static_cast<uint32_t>(bytes[0]) << 16) |
			(static_cast<uint32_t>(bytes[1]) << 8) |
			bytes[2];

	value = raw24 >> 4;

	return true;
}

// POWER uses the entire unsigned 24-bit value.
static bool readRegister24Unsigned(uint8_t reg, uint32_t &value)
{
	uint8_t bytes[3];

	if (!readRegisterBytes(reg, bytes, sizeof(bytes)))
	{
		return false;
	}

	value =
			(static_cast<uint32_t>(bytes[0]) << 16) |
			(static_cast<uint32_t>(bytes[1]) << 8) |
			bytes[2];

	return true;
}

// ENERGY and CHARGE are 40-bit registers.
//
// Five bytes × 8 bits = 40 bits.
//
// Each pass through the loop shifts the number left by one byte and appends
// the next byte.
static bool readRegister40Unsigned(uint8_t reg, uint64_t &value)
{
	uint8_t bytes[5];

	if (!readRegisterBytes(reg, bytes, sizeof(bytes)))
	{
		return false;
	}

	value = 0;

	for (uint8_t byte : bytes)
	{
		value = (value << 8) | byte;
	}

	return true;
}

static bool readRegister40Signed(uint8_t reg, int64_t &value)
{
	uint64_t raw;

	if (!readRegister40Unsigned(reg, raw))
	{
		return false;
	}

	value = signExtend40(raw);

	return true;
}

// ============================================================================
// INA228 IDENTITY CHECK
// ============================================================================

bool verifyInaIdentity()
{
	Serial.println("[INA228] Verifying device identity...");

	uint16_t manufacturerId;
	uint16_t deviceId;

	if (!readRegister16(REG_MANUFACTURER_ID, manufacturerId) ||
			!readRegister16(REG_DEVICE_ID, deviceId))
	{

		Serial.println("[ERROR] Could not read INA228 identity registers.");
		return false;
	}

	Serial.print("[INA228] Manufacturer ID = 0x");
	Serial.println(manufacturerId, HEX);

	Serial.print("[INA228] Device ID       = 0x");
	Serial.println(deviceId, HEX);

	// TI's manufacturer ID is ASCII "TI":
	//
	//   'T' = 0x54
	//   'I' = 0x49
	//
	// giving:
	//
	//   0x5449
	if (manufacturerId != 0x5449)
	{
		Serial.println(
				"[ERROR] Manufacturer ID is not Texas Instruments (0x5449).");
		return false;
	}

	// DEVICE_ID contains:
	//
	// upper 12 bits = device number
	// lower 4 bits  = revision
	//
	// Example:
	//
	//   0x2281
	//
	// means:
	//
	//   device   = 0x228
	//   revision = 1
	uint16_t deviceNumber = deviceId >> 4;
	uint8_t revision = deviceId & 0x0F;

	if (deviceNumber != 0x228)
	{
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

// ============================================================================
// INA228 CONFIGURATION
// ============================================================================

bool configureIna228()
{
	Serial.println("[INA228] Configuring sensor...");

	// CONFIG = 0 keeps ADCRANGE = 0.
	//
	// Our calibrated shunt-voltage scale depends on this.
	if (!writeRegister16(REG_CONFIG, 0x0000))
	{
		return false;
	}

	if (!writeRegister16(REG_ADC_CONFIG, ADC_CONFIG_VALUE))
	{
		return false;
	}

	if (!writeRegister16(REG_SHUNT_CAL, SHUNT_CAL_VALUE))
	{
		return false;
	}

	// Now READ BACK the values we just wrote.
	//
	// This matters because:
	//
	//   "I sent the bytes successfully"
	//
	// is not quite the same proof as:
	//
	//   "The device now contains the configuration I expected."
	uint16_t configReadback;
	uint16_t adcReadback;
	uint16_t shuntCalReadback;

	if (!readRegister16(REG_CONFIG, configReadback) ||
			!readRegister16(REG_ADC_CONFIG, adcReadback) ||
			!readRegister16(REG_SHUNT_CAL, shuntCalReadback))
	{

		Serial.println("[ERROR] Could not read INA228 configuration back.");
		return false;
	}

	// CONFIG bit 4 is ADCRANGE.
	if ((configReadback & 0x0010) != 0)
	{
		Serial.println(
				"[ERROR] INA228 ADCRANGE is not 0. Calibration would be wrong.");
		return false;
	}

	if (adcReadback != ADC_CONFIG_VALUE)
	{
		Serial.print("[ERROR] ADC_CONFIG readback mismatch. Got 0x");
		Serial.print(adcReadback, HEX);
		Serial.print(", expected 0x");
		Serial.println(ADC_CONFIG_VALUE, HEX);
		return false;
	}

	if (shuntCalReadback != SHUNT_CAL_VALUE)
	{
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

// ============================================================================
// INA228 SHUTDOWN MODE
// ============================================================================
//
// Used by the INA-OFF variant of the deep-sleep power test to measure the
// lowest power reachable WITHOUT physically power-gating the INA228.
//
// This never removes INA228 power, never touches the XIAO 3V3 rail, and never
// disconnects SDA or SCL. It writes the documented shutdown MODE and nothing
// else: every other ADC_CONFIG bit is preserved.
//
// MEASUREMENT SEMANTICS, and they are not subtle:
//
//   While the INA228 is shut down, no ADC conversions occur. Voltage,
//   current, power, and temperature registers stop being updated, and the
//   hardware CHARGE and ENERGY accumulators stop accumulating. The registers
//   still read back, but their contents are STALE, not new measurements.
//
// This variant deliberately sacrifices solar measurement during the sleep
// phase. That is the entire point of the experiment.
// ============================================================================

uint16_t inaModeFromAdcConfig(uint16_t adcConfig)
{
	return (adcConfig & INA_MODE_MASK) >> INA_MODE_SHIFT;
}

static bool inaModeIsShutdown(uint16_t mode)
{
	return mode == INA_MODE_SHUTDOWN_ZERO || mode == INA_MODE_SHUTDOWN_EIGHT;
}

static void printAdcConfigHex(const char *label, uint16_t value)
{
	Serial.print(label);
	Serial.print("0x");

	// Print a fixed four-digit hex value. Serial.print(x, HEX) drops leading
	// zeros, which would make 0x0B6B display as "B6B" and read like a
	// different register width.
	for (int8_t nibble = 3; nibble >= 0; nibble--)
	{
		uint8_t digit = (value >> (nibble * 4)) & 0x0F;
		Serial.print(digit, HEX);
	}

	Serial.println();
}

bool enterInaShutdownMode()
{
	Serial.println("[INA228] Entering shutdown mode...");

	uint16_t before;

	if (!readRegister16(REG_ADC_CONFIG, before))
	{
		Serial.println(
				"[INA228] ERROR: Could not read ADC_CONFIG before shutdown.");
		return false;
	}

	printAdcConfigHex("[INA228] ADC_CONFIG before: ", before);

	// Replace ONLY the MODE field. VBUSCT, VSHCT, VTCT, and AVG are carried
	// through untouched so that waking into the normal configuration does not
	// depend on this function having preserved them by accident.
	uint16_t desired =
			(before & ~INA_MODE_MASK) |
			(static_cast<uint16_t>(INA_MODE_SHUTDOWN_ZERO) << INA_MODE_SHIFT);

	if (!writeRegister16(REG_ADC_CONFIG, desired))
	{
		Serial.println(
				"[INA228] ERROR: Could not write ADC_CONFIG shutdown mode.");
		return false;
	}

	uint16_t after;

	if (!readRegister16(REG_ADC_CONFIG, after))
	{
		Serial.println(
				"[INA228] ERROR: Could not read ADC_CONFIG back after shutdown "
				"write.");
		return false;
	}

	printAdcConfigHex("[INA228] ADC_CONFIG after:  ", after);

	uint16_t mode = inaModeFromAdcConfig(after);

	if (!inaModeIsShutdown(mode))
	{
		Serial.print(
				"[INA228] ERROR: MODE bits do not indicate shutdown. MODE = 0x");
		Serial.println(mode, HEX);
		Serial.println(
				"[INA228] Datasheet SLYS021A Table 7-6 documents 0h and 8h as "
				"Shutdown.");
		return false;
	}

	// A shutdown write that also changed conversion times or averaging would
	// mean the next wake restores something other than the known-good
	// configuration. Catch that here rather than discovering it in the data.
	if ((after & ~INA_MODE_MASK) != (before & ~INA_MODE_MASK))
	{
		Serial.println(
				"[INA228] ERROR: Non-MODE ADC_CONFIG bits changed during the "
				"shutdown write.");
		printAdcConfigHex("[INA228] Expected non-MODE bits: ",
											static_cast<uint16_t>(before & ~INA_MODE_MASK));
		printAdcConfigHex("[INA228] Actual non-MODE bits:   ",
											static_cast<uint16_t>(after & ~INA_MODE_MASK));
		return false;
	}

	Serial.println("[INA228] Shutdown mode verified: ALL OK");
	Serial.println(
			"[INA228] Measurement SUSPENDED: no conversions, no CHARGE/ENERGY "
			"accumulation.");
	Serial.println(
			"[INA228] Register contents are now stale and must not be reported as "
			"new measurements.");

	return true;
}

// Read back what the device is actually doing. Nothing here asserts a state;
// it reports the MODE field that was read from the part.
bool inaIsInContinuousMode(bool verbose)
{
	uint16_t adcConfig;

	if (!readRegister16(REG_ADC_CONFIG, adcConfig))
	{
		Serial.println(
				"[INA228] ERROR: Could not read ADC_CONFIG to confirm mode.");
		return false;
	}

	uint16_t mode = inaModeFromAdcConfig(adcConfig);

	if (verbose)
	{
		printAdcConfigHex("[INA228] ADC_CONFIG now:    ", adcConfig);
	}

	return mode == INA_MODE_CONTINUOUS_ALL;
}

// Bring the INA228 back to the project's standard continuous configuration.
//
// The normal wake path already does this, because setup() calls
// configureIna228() on every boot. This exists for the paths that do NOT go
// through a reboot: stopping the test while awake, and the failure path where
// shutdown succeeded but the ESP32 then did not sleep.
bool restoreInaContinuousMode()
{
	if (inaIsInContinuousMode(false))
	{
		Serial.println(
				"[INA228] Continuous measurement already active: ALL OK");
		return true;
	}

	Serial.println(
			"[INA228] INA228 is not in continuous mode. Restoring configuration...");

	if (!configureIna228())
	{
		Serial.println(
				"[INA228] ERROR: Could not restore INA228 configuration.");
		return false;
	}

	if (!inaIsInContinuousMode(true))
	{
		Serial.println(
				"[INA228] ERROR: INA228 still does not report continuous mode after "
				"reconfiguration.");
		return false;
	}

	Serial.println("[INA228] Continuous measurement restored: ALL OK");
	return true;
}

// Print the raw ADC_CONFIG and the decoded MODE field, for the case where the
// device is in some state other than continuous conversion. Saying only "not
// continuous" would leave the operator with nowhere to go.
void printPowerTestInaModeDetail()
{
	uint16_t adcConfig;

	if (!readRegister16(REG_ADC_CONFIG, adcConfig))
	{
		Serial.println(
				"[INA228] ERROR: Could not read ADC_CONFIG for mode detail.");
		return;
	}

	printAdcConfigHex("[INA228] ADC_CONFIG now:    ", adcConfig);

	uint16_t mode = inaModeFromAdcConfig(adcConfig);

	Serial.print("[INA228] MODE field: 0x");
	Serial.print(mode, HEX);

	if (inaModeIsShutdown(mode))
	{
		Serial.println(" (Shutdown)");
		Serial.println(
				"[INA228] Measurement is SUSPENDED. Register values are stale.");
	}
	else if (mode == INA_MODE_CONTINUOUS_ALL)
	{
		Serial.println(" (Continuous bus voltage, shunt voltage and temperature)");
	}
	else
	{
		Serial.println(" (see datasheet SLYS021A Table 7-6)");
	}
}

// ============================================================================
// RESET INA228 ENERGY AND CHARGE ACCUMULATORS
// ============================================================================
//
// CONFIG bit 14 is RSTACC.
//
// Setting that bit tells the INA228:
//
//   "Clear the hardware ENERGY and CHARGE counters."
//
// It does NOT reboot the sensor.
//
// It does NOT erase SHUNT_CAL.
//
// It does NOT erase ADC_CONFIG.
//
// The bit automatically clears itself after performing the reset.
// ============================================================================

bool resetInaAccumulators()
{
	Serial.println(
			"[INA228] Resetting ENERGY and CHARGE accumulators...");

	uint16_t config;

	if (!readRegister16(REG_CONFIG, config))
	{
		Serial.println(
				"[ERROR] Cannot reset accumulators because CONFIG read failed.");
		return false;
	}

	if (!writeRegister16(REG_CONFIG, config | 0x4000))
	{
		Serial.println("[ERROR] Accumulator reset command failed.");
		return false;
	}

	// Give the INA228 a moment to execute the reset.
	delay(2);

	// Prove that the reset actually happened by reading the hardware accumulators
	// back from the INA228.
	uint64_t energyRaw;
	int64_t chargeRaw;

	if (!readRegister40Unsigned(REG_ENERGY, energyRaw) ||
			!readRegister40Signed(REG_CHARGE, chargeRaw))
	{

		Serial.println("[ERROR] Could not verify accumulator reset.");
		return false;
	}

	// Because the ADC is running continuously, it is theoretically possible for
	// accumulation to begin again very quickly.
	//
	// Normally we see exactly zero immediately after reset.
	if (energyRaw != 0 || chargeRaw != 0)
	{
		Serial.print(
				"[WARNING] Accumulator reset readback was not exactly zero. ENERGY=");
		Serial.print(static_cast<unsigned long long>(energyRaw));

		Serial.print(" CHARGE=");
		Serial.println(static_cast<long long>(chargeRaw));
	}
	else
	{
		Serial.println("[INA228] Accumulators reset: OK");
	}

	return true;
}

// ============================================================================
// READ ALL LIVE SENSOR VALUES
// ============================================================================

bool readSensor(SensorReading &reading, bool verboseVshunt)
{
	uint32_t rawBus;
	int32_t rawCurrent;
	uint32_t rawPower;
	uint16_t rawTempUnsigned;
	int32_t rawVshunt;

	if (!readRegister20Unsigned(REG_VBUS, rawBus) ||
			!readRegister20Signed(REG_CURRENT, rawCurrent) ||
			!readRegister24Unsigned(REG_POWER, rawPower) ||
			!readRegister16(REG_DIETEMP, rawTempUnsigned) ||
			!readRegister20Signed(REG_VSHUNT, rawVshunt))
	{

		Serial.println("[ERROR] Sensor measurement read failed.");
		return false;
	}

	// The raw temperature bits represent a signed 16-bit value.
	int16_t rawTemp =
			static_cast<int16_t>(rawTempUnsigned);

	// Raw ADC counts become physical measurements by multiplying by the
	// appropriate datasheet scale.
	reading.voltage_V =
			rawBus * VBUS_LSB_V;

	reading.current_mA =
			rawCurrent *
			CURRENT_LSB_A *
			1000.0;

	reading.power_mW =
			rawPower *
			POWER_LSB_W *
			1000.0;

	reading.temperature_C =
			rawTemp *
			DIETEMP_LSB_C;

	reading.rawVshuntCounts =
			rawVshunt;

	reading.shuntVoltage_mV =
			rawVshunt *
			VSHUNT_LSB_V *
			1000.0;

	// Independent-looking diagnostic calculation:
	//
	//   I = V / R
	//
	// This converts measured shunt voltage into current.
	//
	// Remember that it uses our assumed SHUNT_OHMS value, so this is excellent
	// for detecting internal inconsistencies but is not a completely independent
	// calibration reference.
	reading.currentFromShunt_mA =
			(reading.shuntVoltage_mV / 1000.0) /
			SHUNT_OHMS *
			1000.0;

	if (verboseVshunt)
	{
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

// ============================================================================
// READ INA228 HARDWARE CHARGE ACCUMULATOR
// ============================================================================

bool readAccumulatedCharge_mAh(double &charge_mAh)
{
	int64_t rawCharge;

	if (!readRegister40Signed(REG_CHARGE, rawCharge))
	{
		return false;
	}

	// INA228 CHARGE is stored in coulombs.
	//
	// 1 amp-hour = 3600 coulombs
	// 1 mAh      = 3.6 coulombs
	//
	// Therefore:
	//
	//   mAh = coulombs / 3.6
	charge_mAh =
			rawCharge *
			CHARGE_LSB_C /
			3.6;

	return true;
}

// ============================================================================
// READ INA228 HARDWARE ENERGY ACCUMULATOR
// ============================================================================

bool readAccumulatedEnergy_mWh(double &energy_mWh)
{
	uint64_t rawEnergy;

	if (!readRegister40Unsigned(REG_ENERGY, rawEnergy))
	{
		return false;
	}

	// INA228 ENERGY is stored in joules.
	//
	// 1 Wh  = 3600 joules
	// 1 mWh = 3.6 joules
	energy_mWh =
			rawEnergy *
			ENERGY_LSB_J /
			3.6;

	return true;
}
