// ============================================================================
// HOST CONNECTION TRANSPORTS - implementation
// ============================================================================
//
// Moved out of solar-logger.ino unchanged, apart from `static` on the bitmask
// and on the helper that nothing outside this file calls. See connection.h for
// what the module owns and what it leaves to its callers.
//
// A .cpp is compiled on its own, without the #include <Arduino.h> that Arduino
// CLI prepends to the sketch, so it includes Arduino.h itself for Serial.
// ============================================================================

#include "connection.h"

#include <Arduino.h>

// The empty mask, and the transports nothing claims yet. Private until
// something outside this file uses one (D-047).
constexpr uint8_t CONNECTION_NONE = 0x00;
constexpr uint8_t CONNECTION_WIFI = 0x02;
constexpr uint8_t CONNECTION_BLE = 0x04;

// Which transports are claimed. Only connectionClaim() sets a bit, and only an
// explicit host lease calls it; only connectionRelease() clears one. Callers
// ask anyHostConnected() and never read the mask.
static uint8_t activeTransports = CONNECTION_NONE;

// ============================================================================
// CONNECTION OWNERSHIP
// ============================================================================

bool anyHostConnected()
{
	return activeTransports != CONNECTION_NONE;
}

void printActiveTransports()
{
	Serial.print("[CONNECTION] Active transports: ");

	if (activeTransports == CONNECTION_NONE)
	{
		Serial.println("NONE");
		return;
	}

	bool first = true;

	if (activeTransports & CONNECTION_USB)
	{
		Serial.print("USB");
		first = false;
	}

	if (activeTransports & CONNECTION_WIFI)
	{
		if (!first)
			Serial.print("+");
		Serial.print("WIFI");
		first = false;
	}

	if (activeTransports & CONNECTION_BLE)
	{
		if (!first)
			Serial.print("+");
		Serial.print("BLE");
		first = false;
	}

	Serial.println();
}

static const char *transportName(uint8_t transport)
{
	switch (transport)
	{
	case CONNECTION_USB:
		return "USB";
	case CONNECTION_WIFI:
		return "WIFI";
	case CONNECTION_BLE:
		return "BLE";
	default:
		return "UNKNOWN";
	}
}

void connectionClaim(uint8_t transport)
{
	const uint8_t before = activeTransports;

	activeTransports |= transport;

	if (before != activeTransports)
	{
		Serial.print("[CONNECTION] ");
		Serial.print(transportName(transport));
		Serial.println(" transport CLAIMED by an explicit host lease.");
		printActiveTransports();
		Serial.println("[CONNECTION] Any host connected: YES");
	}
}

void connectionRelease(uint8_t transport)
{
	const uint8_t before = activeTransports;

	activeTransports &= (uint8_t)~transport;

	if (before != activeTransports)
	{
		Serial.print("[CONNECTION] ");
		Serial.print(transportName(transport));
		Serial.println(" transport RELEASED.");
		printActiveTransports();
		Serial.print("[CONNECTION] Any host connected: ");
		Serial.println(anyHostConnected() ? "YES" : "NO");
	}
}
