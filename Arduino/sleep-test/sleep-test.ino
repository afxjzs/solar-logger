// ESP32 sleep functions live in this header.
#include "esp_sleep.h"

// ============================================================================
// SLEEP TIME
// ============================================================================

// We will sleep for 10 seconds for this bench experiment.
//
// Later, this could be:
//
//     15 minutes
//     30 minutes
//     1 hour
//
// We use uint64_t because microsecond values can become very large.
constexpr uint64_t SLEEP_SECONDS = 10;


// ============================================================================
// SETUP
// ============================================================================

// IMPORTANT:
//
// After deep sleep, the ESP32 boots again.
//
// That means setup() runs again from the beginning.
//
// It does NOT resume from the line after esp_deep_sleep_start().
void setup() {

  // Start the USB serial connection.
  Serial.begin(115200);

  // Give the serial connection time to become available.
  delay(2000);

  Serial.println();
  Serial.println("ESP32 deep-sleep test");
  Serial.println("---------------------");


  // ==========================================================================
  // FIND OUT WHY WE WOKE UP
  // ==========================================================================

  // The ESP32 remembers the reason that caused the most recent wake-up.
  //
  // This function returns a value from the esp_sleep_wakeup_cause_t enum.
  //
  // An enum is basically a set of named integer values.
  esp_sleep_wakeup_cause_t wakeupCause =
      esp_sleep_get_wakeup_cause();


  // If the timer caused this boot, print that.
  if (wakeupCause == ESP_SLEEP_WAKEUP_TIMER) {

    Serial.println("Wake reason: TIMER");

  } else {

    // The very first time we plug the board into USB, there was no
    // deep-sleep wake event.
    //
    // It simply powered on normally.
    Serial.println("Wake reason: normal power-on/reset");
  }


  // ==========================================================================
  // DO OUR WORK
  // ==========================================================================

  // For this demonstration, "our work" is just printing a message.
  //
  // In the real solar logger, THIS is where we will:
  //
  //     1. Read the INA228
  //     2. Read its accumulated charge
  //     3. Read its accumulated energy
  //     4. Store the measurement
  //     5. Maybe connect to Wi-Fi
  //     6. Upload data
  //
  Serial.println("ESP32 is awake and doing its work.");

  delay(1000);


  // ==========================================================================
  // CONFIGURE THE NEXT WAKE-UP
  // ==========================================================================

  // The ESP32 timer expects MICROSECONDS rather than seconds.
  //
  // There are:
  //
  //     1,000,000 microseconds in one second.
  //
  // So:
  //
  //     10 seconds
  //
  // becomes:
  //
  //     10 × 1,000,000
  //
  // = 10,000,000 microseconds
  uint64_t sleepMicroseconds =
      SLEEP_SECONDS * 1000000ULL;


  // Tell the ESP32's low-power timer:
  //
  //     "After this many microseconds, wake the chip."
  //
  // ULL means "unsigned long long."
  //
  // It forces the large integer constant to be treated as a 64-bit
  // unsigned number.
  esp_sleep_enable_timer_wakeup(sleepMicroseconds);


  Serial.print("Going to sleep for ");
  Serial.print(SLEEP_SECONDS);
  Serial.println(" seconds.");

  // Give Serial time to finish transmitting before we turn most of
  // the ESP32 off.
  Serial.flush();


  // ==========================================================================
  // ENTER DEEP SLEEP
  // ==========================================================================

  // This is effectively the end of this boot cycle.
  //
  // The CPU shuts down here.
  //
  // Ten seconds later, the low-power timer wakes the chip and setup()
  // begins again from the top.
  esp_deep_sleep_start();


  // NOTHING BELOW THIS LINE WILL RUN.
  //
  // Deep sleep does not return from esp_deep_sleep_start().
}


// ============================================================================
// LOOP
// ============================================================================

// We don't need loop() for this architecture.
//
// Everything happens once per wake cycle inside setup().
void loop() {
}