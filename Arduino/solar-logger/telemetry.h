// ============================================================================
// TELEMETRY
// ============================================================================
//
// The machine-readable CSV lines this firmware prints, and nothing else. This
// module knows HOW a line is spelled: its prefix, its fields, their order,
// their precision, and the commas between them. It never decides WHAT
// happened, WHEN a line should be emitted, or which values belong to the event
// it describes. Those are the caller's, in solar-logger.ino (D-047).
//
// Concretely, this module does not know that an interval lasts sixty seconds,
// that a sample is taken once a second, that a deep-sleep test suspends the
// sample row, or that an experiment can be reset. It is handed finished
// numbers and prints them.
//
// THE WIRE FORMAT IS A CONTRACT. app/solar_logger.py parses these lines by
// position, writes them into data/samples.csv, data/intervals.csv and
// data/events.csv, and refuses a row whose field count is wrong. Changing a
// prefix, a field, an order or a precision changes the host schema, and
// tests/test_characterization_telemetry.py holds the exact bytes so that a
// change here cannot be silent.
//
// CSV_HEADER describes CSV_DATA. They are one fact and must move together.
//
// NOT TELEMETRY: CMD_ACK and CMD_RESULT. Those are the machine command
// protocol, they are written through the bounded protocol writer with its own
// 100 ms deadline (D-036), and they stay in solar-logger.ino. Telemetry uses
// ordinary Serial output, which under Serial.setTxTimeoutMs(0) is dropped
// rather than waited on when no host is draining the port (D-027). The two
// have different delivery guarantees on purpose, and neither may acquire the
// other's.
//
// A CALLER PASSES VALUES, NOT STATE. Everything printed here arrives as a
// parameter. This module holds no mutable state of its own, reads no global,
// and depends on nothing in the project: the experiment id, the interval
// number and the running totals all live in solar-logger.ino, which owns when
// they change. That is why the structs below repeat the measurement fields
// rather than taking a SensorReading, which would make telemetry depend on the
// INA228 driver for four doubles it only prints.
// ============================================================================

#pragma once

#include <stdint.h>

// ----------------------------------------------------------------------------
// The two row shapes
// ----------------------------------------------------------------------------
//
// Each struct's members are in wire order, so the CSV line and the type that
// describes it cannot drift apart while anyone is reading either one. They are
// plain values with no constructor, so a caller can brace-initialize one in
// field order and the compiler checks the count.

// One instantaneous high-resolution measurement: CSV_SAMPLE.
struct TelemetrySample
{
	uint32_t experimentId;
	double elapsedSeconds;
	double voltage_V;
	double current_mA;
	double power_mW;
	double temperature_C;
};

// One completed accounting interval: CSV_DATA, the authoritative summary.
//
// The running totals are the experiment's, not the interval's, and they are
// passed in for the same reason as everything else here: this module prints
// them and never advances them.
struct TelemetryInterval
{
	uint32_t experimentId;
	uint32_t interval;
	double elapsedSeconds;
	double voltage_V;
	double current_mA;
	double power_mW;
	double temperature_C;
	double intervalCharge_mAh;
	double intervalEnergy_mWh;
	double averageCurrent_mA;
	double averagePower_mW;
	double runningCharge_mAh;
	double runningEnergy_mWh;
};

// ----------------------------------------------------------------------------
// Emission
// ----------------------------------------------------------------------------

// The CSV_HEADER line naming every CSV_DATA field, preceded by the blank line
// and the human-readable announcement that have always accompanied it.
void telemetryPrintHeader();

void telemetryPrintSample(const TelemetrySample &sample);

void telemetryPrintInterval(const TelemetryInterval &interval);

// ----------------------------------------------------------------------------
// Events
// ----------------------------------------------------------------------------
//
// Three functions rather than one TelemetryEvent, because the three lines are
// genuinely different shapes and the host's parser depends on that: it splits
// an event into a type, an experiment id, and a tail it keeps whole.
// EXPERIMENT_START has no tail at all, which is why the host's minimum is
// three fields. One struct with an optional detail would have to invent a way
// to say "no tail", and would describe the wire format less honestly than
// these do.

void telemetryPrintExperimentStart(uint32_t experimentId);

void telemetryPrintExperimentResume(uint32_t experimentId,
																		uint32_t completedInterval);

// A deep-sleep power-test marker. `eventType` is the event name the caller
// chose; this module does not know the set of them.
//
// Durability caveat, and it is a real one: a WAKE marker is emitted during
// setup(), before the host has finished rediscovering the re-enumerated USB
// device, so it is normally LOST. That is the same known limitation already
// documented for EXPERIMENT_RESUME and is one of the reasons the
// store-and-forward item exists in docs/BACKLOG.md.
void telemetryPrintSleepTestEvent(const char *eventType,
																	uint32_t experimentId,
																	uint32_t cycle);
