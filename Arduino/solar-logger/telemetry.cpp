#include "telemetry.h"

// A .cpp does not get the <Arduino.h> that Arduino CLI prepends to the sketch,
// so this file includes what it uses: Serial (D-047).
#include <Arduino.h>

// ============================================================================
// CSV OUTPUT
// ============================================================================
//
// CSV is PRINTED through Serial.
//
// It is not stored as a CSV file inside the ESP32.
//
// Each data row contains experiment_id.
//
// That makes this safe:
//
//   CSV_DATA,1,1,...
//   CSV_DATA,1,2,...
//   CSV_DATA,1,3,...
//
//   RESET YES
//
//   CSV_DATA,2,1,...
//   CSV_DATA,2,2,...
//
// Later we can simply select:
//
//   experiment_id == 2
//
// and know exactly which rows belong to that experiment.
//
//
// WHY THE PRECISIONS ARE WHAT THEY ARE
//
// Each Serial.print(value, digits) call below names the number of decimal
// places for one field, and they are deliberately not all the same:
//
//   3   elapsed_seconds        milliseconds are the finest thing measured
//   6   volts, milliamps,      the INA228's own resolution is well inside
//       milliwatts             this, and the host plots these directly
//   4   temperature_C          the die sensor's LSB is 7.8125 mC
//   9   charge and energy      one 60-second interval's share of a
//                              milliamp-hour is small, and the running totals
//                              are built by adding those small numbers up
//
// Arduino's Print does not use printf. It adds half of the last requested
// place and then truncates one digit at a time, so these figures are what the
// board actually emits rather than what a format string would produce.
// ============================================================================

void telemetryPrintHeader()
{
	Serial.println();

	Serial.println(
			"[CSV] Machine-readable interval logging enabled.");

	Serial.println(
			"CSV_HEADER,"
			"experiment_id,"
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

// Machine-readable high-resolution telemetry.
//
// Format:
//
// CSV_SAMPLE,
// experiment_id,
// elapsed_seconds,
// voltage_V,
// current_mA,
// power_mW,
// temperature_C
//
void telemetryPrintSample(const TelemetrySample &sample)
{
	Serial.print("CSV_SAMPLE,");

	Serial.print(sample.experimentId);

	Serial.print(',');
	Serial.print(sample.elapsedSeconds, 3);

	Serial.print(',');
	Serial.print(sample.voltage_V, 6);

	Serial.print(',');
	Serial.print(sample.current_mA, 6);

	Serial.print(',');
	Serial.print(sample.power_mW, 6);

	Serial.print(',');
	Serial.println(sample.temperature_C, 4);
}

void telemetryPrintInterval(const TelemetryInterval &interval)
{

	Serial.print("CSV_DATA,");

	Serial.print(interval.experimentId);

	Serial.print(',');
	Serial.print(interval.interval);

	Serial.print(',');
	Serial.print(interval.elapsedSeconds, 3);

	Serial.print(',');
	Serial.print(interval.voltage_V, 6);

	Serial.print(',');
	Serial.print(interval.current_mA, 6);

	Serial.print(',');
	Serial.print(interval.power_mW, 6);

	Serial.print(',');
	Serial.print(interval.temperature_C, 4);

	Serial.print(',');
	Serial.print(interval.intervalCharge_mAh, 9);

	Serial.print(',');
	Serial.print(interval.intervalEnergy_mWh, 9);

	Serial.print(',');
	Serial.print(interval.averageCurrent_mA, 6);

	Serial.print(',');
	Serial.print(interval.averagePower_mW, 6);

	Serial.print(',');
	Serial.print(interval.runningCharge_mAh, 9);

	Serial.print(',');
	Serial.println(interval.runningEnergy_mWh, 9);
}

// ============================================================================
// EVENTS
// ============================================================================

// Explicit machine-readable event showing that a NEW experiment started.
void telemetryPrintExperimentStart(uint32_t experimentId)
{
	Serial.print("CSV_EVENT,EXPERIMENT_START,");
	Serial.println(experimentId);
}

// Explicit machine-readable event showing that an existing experiment was
// resumed after an ESP32 reboot.
void telemetryPrintExperimentResume(uint32_t experimentId,
																		uint32_t completedInterval)
{
	Serial.print("CSV_EVENT,EXPERIMENT_RESUME,");
	Serial.print(experimentId);
	Serial.print(',');
	Serial.println(completedInterval);
}

// Machine-readable markers for the deep-sleep power test.
//
// The host parser already treats everything after experiment_id as one
// comma-joined detail field, so these need no change on the Python side. The
// durability caveat is in telemetry.h, where a caller will see it.
void telemetryPrintSleepTestEvent(const char *eventType,
																	uint32_t experimentId,
																	uint32_t cycle)
{
	Serial.print("CSV_EVENT,");
	Serial.print(eventType);
	Serial.print(',');
	Serial.print(experimentId);
	Serial.print(',');
	Serial.println(cycle);
}
