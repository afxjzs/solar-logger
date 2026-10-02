// ============================================================================
// RTC-RETAINED AUTONOMOUS STATE - definitions
// ============================================================================
//
// Moved out of solar-logger.ino on 2026-10-02 unchanged. These are the only
// definitions; the header declares them `extern`. See
// autonomous_retained_state.h for what the module owns and what it does not.
//
// A .cpp is compiled on its own, without the #include <Arduino.h> that Arduino
// CLI prepends to the sketch, so it includes esp_attr.h itself for
// RTC_DATA_ATTR.
// ============================================================================

#include "autonomous_retained_state.h"

#include <esp_attr.h>

RTC_DATA_ATTR uint32_t rtcAutoMagic = 0;
RTC_DATA_ATTR uint32_t rtcAutoBootId = 0;
RTC_DATA_ATTR uint32_t rtcAutoSessionElapsedMs = 0;
RTC_DATA_ATTR uint32_t rtcAutoIntervalStartMs = 0;
RTC_DATA_ATTR uint32_t rtcAutoNextSeq = 0;
RTC_DATA_ATTR uint32_t rtcAutoSeqHighWater = 0;
RTC_DATA_ATTR int64_t rtcAutoRunChargeUAh = 0;
RTC_DATA_ATTR int64_t rtcAutoRunEnergyUWh = 0;
RTC_DATA_ATTR uint32_t rtcAutoCycleCount = 0;
RTC_DATA_ATTR uint32_t rtcAutoCommandedSleepMs = 0;
