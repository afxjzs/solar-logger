// ============================================================================
// NVS PERSISTENCE
// ============================================================================
//
// The mechanics of reading and writing ESP32 nonvolatile storage, and nothing
// else. This module knows HOW to save and load a value. It never decides WHEN
// or WHY a value should change: arming, disarming, closing an interval,
// starting an experiment and reserving a sequence block are all decided in
// solar-logger.ino, which calls down into this module (D-047).
//
// The name says NVS rather than "persistence" because this board has three
// different durable stores and they keep different promises:
//
//   NVS         survives power loss                     THIS MODULE
//   RTC memory  survives deep sleep, lost on power loss  solar-logger.ino
//   LittleFS    the durable record log                   solar-logger.ino
//
// WHAT IS PRIVATE. The single Preferences object, the "solarlog" namespace and
// every key string live in nvs_persistence.cpp. Nothing outside it names a key
// or opens the namespace, so the stored layout cannot drift out of one file.
//
// FAILURE IS REPORTED, NEVER DEFAULTED. Every function that can fail returns
// whether it succeeded, and callers must not report success without it. In
// particular loadExperimentId() returns false rather than yielding a plausible
// experiment 0: D-024 requires an unknown attribution to be marked, and
// docs/BACKLOG.md records the defect that rule was written from.
//
// TWO KNOWN DEFECTS ARE PRESERVED HERE, not introduced by the extraction and
// deliberately not fixed inside a move. Both are written up in
// docs/BACKLOG.md under "NVS load failures become plausible defaults with no
// signal":
//
//   loadAutonomousConfig()   reports success after a failed open, having
//                            produced "not armed" and the default cadence
//   loadSequenceHighWater()  returns 0 after a failed open, which reads the
//                            same as "nothing was ever reserved"
//
// Fixing either changes behavior and belongs in its own change.
// ============================================================================

#pragma once

#include <stdint.h>

// ----------------------------------------------------------------------------
// The experiment checkpoint
// ----------------------------------------------------------------------------
//
// One saved checkpoint, as five NVS keys hold it: schema, experiment,
// interval, charge and energy. The schema is not a field here because it
// belongs to the stored format rather than to the experiment, and this module
// is the only thing that may choose it.
//
// This is the transport between the caller's experiment state and NVS. The
// experiment globals themselves stay in solar-logger.ino, which owns when they
// change; they move in the `experiment` stage of the plan in docs/BACKLOG.md.
struct LoggerCheckpoint
{
	uint32_t experimentId;
	uint32_t completedInterval;
	double runningCharge_mAh;
	double runningEnergy_mWh;
};

// Write the whole checkpoint. Every key is attempted even after one fails, so
// the console names every key that did not write, and the return value is
// false if any of them did not.
bool saveCheckpoint(const LoggerCheckpoint &checkpoint);

// Read the checkpoint, understanding schema 1 and schema 2.
//
// true  - `out` holds the stored state, or zeros for a store that has never
//         been written. Every field is assigned on every true return.
// false - the stored state could not be interpreted: an incomplete checkpoint
//         or an unsupported schema. `out` is not written, and the caller must
//         not treat it as a fresh experiment.
bool loadCheckpoint(LoggerCheckpoint &out);

// Read only the experiment id, quietly and read-only, for the autonomous wake
// path that runs before loadCheckpoint() (D-024).
//
// false means the id is UNKNOWN. A caller must never turn that into 0.
bool loadExperimentIdOnly(uint32_t &outExperimentId);

// ----------------------------------------------------------------------------
// Power-test flags
// ----------------------------------------------------------------------------
//
// Three flags under three keys, deliberately separate from the experiment
// checkpoint so that arming a bench test can never alter experiment totals
// (D-006, D-011, D-014). This module stores and reports them; what an armed
// flag then means, and which combinations are refused, stays with the power
// test in solar-logger.ino.
//
// A flag that cannot be read is reported as not armed, and the loader says so
// on the console rather than leaving the caller's variable at whatever it
// held.

bool saveWifiPowerTestArmed(bool armed);
bool loadWifiPowerTestArmed(bool &outArmed);

bool saveSleepPowerTestArmed(bool armed);
bool loadSleepPowerTestArmed(bool &outArmed);

bool saveSleepPowerTestInaOff(bool inaOff);
bool loadSleepPowerTestInaOff(bool &outInaOff);

// ----------------------------------------------------------------------------
// Autonomous mode
// ----------------------------------------------------------------------------

// The persisted armed flag. RAM follows NVS: a caller clears its own copy only
// after this reports success.
bool saveAutonomousTestArmed(bool armed);

// Read the armed flag and the stored cadence. `defaultIntervalSeconds` is used
// for a cadence that is absent or unreadable; the supported range and the
// refusal to run outside it are cadence policy (D-018) and stay with the
// caller, which is why the bounds are not named here.
//
// Returns true today on every path, including a namespace that would not open.
// See the defect note at the top of this header.
bool loadAutonomousConfig(bool &outArmed,
													uint32_t &outIntervalSeconds,
													uint32_t defaultIntervalSeconds);

bool saveAutonomousInterval(uint32_t seconds);

// ----------------------------------------------------------------------------
// Sequence reservation and boot id
// ----------------------------------------------------------------------------

// The stored sequence high-water mark. Whether the durable log tail or this
// reservation is authoritative is D-023 policy and stays in the caller; this
// module only stores and reports the number.
//
// A read that fails returns 0, which is indistinguishable from "never
// reserved". See the defect note at the top of this header.
uint32_t loadSequenceHighWater();
bool saveSequenceHighWater(uint32_t highWater);

// Increment and return the persisted boot id, which identifies one continuous
// power-on session. Returns 0 and says so on the console when NVS cannot be
// opened; 0 is not unique, and the caller is told rather than left to assume.
//
// When a boot id is consumed, and the RTC-retained copy of it, belong to the
// autonomous state machine in solar-logger.ino.
uint32_t nextBootId();
