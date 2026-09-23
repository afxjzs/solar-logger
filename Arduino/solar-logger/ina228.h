// ============================================================================
// INA228 DRIVER
// ============================================================================
//
// Hardware access to the Texas Instruments INA228 at I2C address 0x40: register
// reads and writes, identity, configuration, shutdown and continuous mode,
// measurement reads, and the CHARGE/ENERGY accumulator primitives.
//
// The driver exposes mechanics. It never decides WHEN to read, reset, or
// reconfigure. When the accumulators are reset, which interval owns their
// charge, and when a baseline is established all stay with the callers in
// solar-logger.ino (D-020, D-026, D-028).
//
// This header holds only what code outside ina228.cpp uses. Register
// addresses, scale factors, and helpers used only inside the driver are
// private to ina228.cpp.
//
// Deliberately NOT here: inaShutdownActive. The power test sets and clears
// it, and STATUS and the heartbeat read it. The driver does neither, so the
// flag stays with the code that owns it.
//
// PRECONDITION: the caller starts the bus with Wire.begin() before calling
// anything here. setup() and runAutonomousWakeCycle() each do that
// themselves, because bus bring-up is part of boot and wake ordering.
//
// Every register access goes through readRegisterBytes() or writeRegister16()
// in ina228.cpp, and both print an I2C failure on Serial where it happens.
// ============================================================================

#pragma once

#include <stdint.h>

// ============================================================================
// INA228 REGISTER ADDRESSES
// ============================================================================
//
// The INA228 contains a collection of internal registers.
//
// Think of them as numbered storage locations inside the chip.
//
// Some registers contain configuration:
//   ADC_CONFIG
//   SHUNT_CAL
//
// Some registers contain measurements:
//   VBUS
//   VSHUNT
//   CURRENT
//   POWER
//
// Some registers are accumulators:
//   ENERGY
//   CHARGE
//
// I2C lets us tell the chip:
//   "Give me the bytes stored at register 0x05"
// and then read those bytes back.
// ============================================================================

constexpr uint8_t REG_CONFIG = 0x00;
constexpr uint8_t REG_ADC_CONFIG = 0x01;
constexpr uint8_t REG_SHUNT_CAL = 0x02;

// Diagnostic flags and alert register. Datasheet SLYS021A Table 7-16.
// (An earlier revision of this comment cited Table 7-9, which is a different
// register. Verified against SLYS021A, January 2021, revised May 2022.)
//
// The three bits that decide whether a completed interval can be trusted, with
// the datasheet's clear conditions quoted because they are the whole problem:
//
//   bit 11  ENERGYOF   ENERGY register overflowed.
//                      "Clears when the ENERGY register is read."
//   bit 10  CHARGEOF   CHARGE register overflowed.
//                      "Clears when the CHARGE register is read."
//   bit  9  MATHOF     arithmetic overflow; current and power may be invalid.
//                      "Must be manually cleared by triggering another
//                      conversion or by clearing the accumulators with the
//                      RSTACC bit."
//
// CONSEQUENCE, AND THE RULE FOR EVERY CALLER. Reading ENERGY or CHARGE destroys
// its own overflow flag. Sampling DIAG_ALRT in the same wake is NOT sufficient:
// it must be sampled BEFORE the accumulator reads, or ENERGYOF and CHARGEOF are
// already zero and read as "no overflow".
//
// This was a live defect until 2026-09-23: runAutonomousWakeCycle() read the
// accumulators first, so AUTO_FLAG_INA_ACCUM_OF could never be set by a real
// overflow and every record claimed a clean interval. The order is now DIAG_ALRT
// first, and test_characterization_record.py pins it so a later file move cannot
// quietly restore the old order.
//
// Any NEW caller that reads these accumulators owns the same ordering
// obligation. closeMeasurementInterval(), the tethered interval close, still
// does not read DIAG_ALRT at all - a known gap, not a false claim, tracked in
// docs/BACKLOG.md. See also docs/DECISIONS.md D-020.
//
// MATHOF is not affected by this ordering, because a read does not clear it.
// In continuous conversion mode it is cleared by the next conversion instead,
// so it qualifies roughly the latest conversion rather than the whole interval.
constexpr uint8_t REG_DIAG_ALRT = 0x0B;

constexpr uint16_t DIAG_ENERGYOF_MASK = 0x0800;
constexpr uint16_t DIAG_CHARGEOF_MASK = 0x0400;
constexpr uint16_t DIAG_MATHOF_MASK = 0x0200;

constexpr uint8_t REG_MANUFACTURER_ID = 0x3E;
constexpr uint8_t REG_DEVICE_ID = 0x3F;

// ============================================================================
// KNOWN-GOOD INA228 CONFIGURATION
// ============================================================================
//
// These values come from the bench configuration we already validated.
//
// ADC_CONFIG = 0xFB6B
//   Continuous bus-voltage, shunt-voltage, and temperature measurement.
//   64-sample averaging.
//
// SHUNT_CAL = 819
//   Calibration value for our effective ~15.62 mOhm shunt.
//
// CURRENT_LSB = 4 uA/count
//   One raw CURRENT-register count represents 4 microamps.
//
// ADCRANGE = 0
//   VSHUNT resolution is 312.5 nV/count.
// ============================================================================

constexpr uint16_t ADC_CONFIG_VALUE = 0xFB6B;
constexpr uint16_t SHUNT_CAL_VALUE = 819;

// ============================================================================
// INA228 ADC_CONFIG MODE FIELD
// ============================================================================
//
// Source: TI INA228 datasheet SLYS021A (January 2021, revised May 2022),
// Table 7-6, "ADC_CONFIG Register Field Descriptions". Address 1h,
// reset = FB68h.
//
//   Bits 15-12  MODE     (reset Fh)
//   Bits 11-9   VBUSCT   (reset 5h = 1052 us)
//   Bits 8-6    VSHCT    (reset 5h = 1052 us)
//   Bits 5-3    VTCT     (reset 5h = 1052 us)
//   Bits 2-0    AVG      (reset 0h = 1 sample)
//
// The MODE enumeration from that table:
//
//   0h = Shutdown
//   1h = Triggered bus voltage, single shot
//   2h = Triggered shunt voltage, single shot
//   3h = Triggered shunt voltage and bus voltage, single shot
//   4h = Triggered temperature, single shot
//   5h = Triggered temperature and bus voltage, single shot
//   6h = Triggered temperature and shunt voltage, single shot
//   7h = Triggered bus voltage, shunt voltage and temperature, single shot
//   8h = Shutdown
//   9h = Continuous bus voltage only
//   Ah = Continuous shunt voltage only
//   Bh = Continuous shunt and bus voltage
//   Ch = Continuous temperature only
//   Dh = Continuous bus voltage and temperature
//   Eh = Continuous temperature and shunt voltage
//   Fh = Continuous bus voltage, shunt voltage and temperature
//
// NOTE: the datasheet documents TWO shutdown encodings, 0h and 8h. Our
// shutdown verification accepts either, because the test that matters is
// "does the device report a documented shutdown mode", not "does it echo the
// exact bit pattern we happened to choose".
//
// Decoding our own ADC_CONFIG_VALUE of 0xFB6B against that table:
//
//   MODE   = Fh  continuous bus voltage, shunt voltage and temperature
//   VBUSCT = 5h  1052 us
//   VSHCT  = 5h  1052 us
//   VTCT   = 5h  1052 us
//   AVG    = 3h  64 samples
//
// which is exactly what the KNOWN-GOOD block above claims it is.
//
// The MODE field's mask, shift and shutdown encodings are private to
// ina228.cpp. Callers decode the field with inaModeFromAdcConfig().

constexpr uint16_t INA_MODE_CONTINUOUS_ALL = 0xF;

// The CURRENT, VBUS, DIETEMP, POWER, ENERGY and CHARGE scale factors are
// private to ina228.cpp. These two are also printed in setup()'s boot summary.
constexpr double SHUNT_OHMS = 0.01562;

constexpr double VSHUNT_LSB_V = 312.5e-9;

// ADCRANGE = 0 gives us a +/-163.84 mV shunt range.
//
// We use this only for the diagnostic:
//   "Shunt range used: X %"
constexpr double VSHUNT_FULL_SCALE_V = 0.16384;

// ============================================================================
// ONE COMPLETE SENSOR READING
// ============================================================================
//
// A struct is simply a way to group related variables together.
//
// Rather than passing five separate variables around:
//
//   voltage
//   current
//   power
//   temperature
//   shunt voltage
//
// we can pass one SensorReading object.
// ============================================================================

struct SensorReading
{
	double voltage_V;
	double current_mA;
	double power_mW;
	double temperature_C;

	int32_t rawVshuntCounts;
	double shuntVoltage_mV;
	double currentFromShunt_mA;
};

// ============================================================================
// PUBLIC API
// ============================================================================

// Raw 16-bit register read. Exported for the read-only checks that stay with
// their callers: validateInaForWake() and the wake cycle's DIAG_ALRT read.
bool readRegister16(uint8_t reg, uint16_t &value);

// Identity, configuration, and the ADC_CONFIG MODE field.
bool verifyInaIdentity();
bool configureIna228();
uint16_t inaModeFromAdcConfig(uint16_t adcConfig);

// Shutdown and continuous conversion (D-015, D-016).
bool enterInaShutdownMode();
bool inaIsInContinuousMode(bool verbose);
bool restoreInaContinuousMode();
void printPowerTestInaModeDetail();

// Measurements and the hardware accumulators.
bool resetInaAccumulators();
bool readSensor(SensorReading &reading, bool verboseVshunt);
bool readAccumulatedCharge_mAh(double &charge_mAh);
bool readAccumulatedEnergy_mWh(double &energy_mWh);
