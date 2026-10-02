// ============================================================================
// RTC-RETAINED AUTONOMOUS STATE
// ============================================================================
//
// The values the autonomous logger carries across deep sleep, and the magic
// that says whether they are valid. Retained across deep sleep, lost on power
// loss, which is exactly the lifetime of a measurement session. Guarded by a
// magic value because RTC memory contents are undefined after a cold boot
// (D-011).
//
// Everything that must survive power loss lives in NVS or in the durable log
// instead.
//
// WHAT THIS MODULE OWNS. Exactly one definition of each value, in exactly one
// translation unit: autonomous_retained_state.cpp. A definition in a header
// would be the hazard BACKLOG calls the single highest-risk detail of the
// modularization plan: a `static` one gives every translation unit that
// includes it its own silent copy, and the session clock would reset without
// a word.
//
// WHAT IT DOES NOT OWN. Any transition. Every writer stays where it was in
// solar-logger.ino: the scheduler, arm and stop, the host handoff, the wake
// cycle and boot recovery. Reservation policy writes the mirror
// rtcAutoSeqHighWater; this module only gives it a retained home (D-063).
// Power-test RTC state is not here either: rtcSleepTestMagic and
// rtcSleepTestCycle are guarded by their own magic, and stopAutonomousTest()
// clearing rtcAutoMagic must never sit next to them.
//
// WHY `extern`. All ten are exported because code outside this module, the
// sketch, uses all ten, which is what D-047 asks a header to declare. That the
// module writes none of them departs from D-047's "a variable moves into a
// module only if that module writes it". The departure is deliberate and is
// recorded in D-063; tests/test_characterization_autonomous_retained_state.py
// pins every writer of every value in its place.
//
// AUTO_RTC_MAGIC is defined here because the sketch reads it. A namespace-scope
// constexpr has internal linkage, so each translation unit gets its own copy of
// a constant; that is harmless and is not the `static` variable hazard above.
// ============================================================================

#pragma once

#include <stdint.h>

constexpr uint32_t AUTO_RTC_MAGIC = 0xA07011E5;

extern uint32_t rtcAutoMagic;
extern uint32_t rtcAutoBootId;
extern uint32_t rtcAutoSessionElapsedMs;
extern uint32_t rtcAutoIntervalStartMs;
extern uint32_t rtcAutoNextSeq;
extern uint32_t rtcAutoSeqHighWater;
extern int64_t rtcAutoRunChargeUAh;
extern int64_t rtcAutoRunEnergyUWh;
extern uint32_t rtcAutoCycleCount;
extern uint32_t rtcAutoCommandedSleepMs;
