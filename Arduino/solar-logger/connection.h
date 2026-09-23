// ============================================================================
// HOST CONNECTION TRANSPORTS
// ============================================================================
//
// Which transport a host has claimed, and nothing else. This module does not
// know what a lease is, how long one lasts, whether the board may sleep, or
// when autonomous mode resumes. The host session and the autonomous state
// machine in solar-logger.ino decide all of that and call down into this
// module; it never calls up (D-025, D-047).
//
// A transport becomes active only through an explicit claim, which today means
// LOGGER SESSION HOLD. USB being plugged in is not a claim, and neither is a
// terminal holding the port open. Serial.isPlugged() and Serial.isConnected()
// are printed as diagnostics by the sketch and decide nothing: the 2026-09-11
// audit of both forced that distinction, and it is also what lets a future BLE
// client take part on equal terms. So this is a valid, observed state:
//
//   USB plugged: YES   CDC connected: YES   Active transports: NONE
//
// Sleep policy asks anyHostConnected(), never which transport and never
// whether USB is present. USB is the only transport implemented today. Wi-Fi
// and BLE have bits so that they can hold the board awake later without the
// autonomous state machine learning about them.
//
// This header holds only what code outside connection.cpp uses (D-047). The
// NONE, WIFI and BLE values and the bitmask itself are private to
// connection.cpp. A Wi-Fi or BLE value moves here when something claims it.
//
// Every change prints [CONNECTION] lines on Serial. Claiming a transport that
// is already claimed, or releasing one that is not, changes nothing and prints
// nothing.
// ============================================================================

#pragma once

#include <stdint.h>

// The one transport anything claims today, through LOGGER SESSION HOLD.
constexpr uint8_t CONNECTION_USB = 0x01;

// Whether any transport is claimed. The question sleep policy asks (D-025).
bool anyHostConnected();

// "[CONNECTION] Active transports: " then NONE, or the claimed transports
// joined by '+', for example USB or USB+WIFI.
void printActiveTransports();

// Claim or release one transport. Only that transport's bit changes.
void connectionClaim(uint8_t transport);
void connectionRelease(uint8_t transport);
