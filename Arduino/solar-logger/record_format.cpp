#include "record_format.h"

// A .cpp does not get the <Arduino.h> that Arduino CLI prepends to the sketch,
// so this file includes what it uses (D-047). It needs no Arduino.h: nothing
// here prints, reads a clock, or touches a peripheral.
//
// esp_rom_crc32_le() is the ESP32 mask ROM's CRC-32, so the checksum costs no
// flash for a table and no cycles for a bit loop. The board has computed every
// stored record's CRC with it since Experiment 3 began; see record_format.h for
// what proves it is standard IEEE 802.3 CRC-32.
#include "esp_rom_crc.h"

uint32_t autoRecordCrc(const AutoRecord &record)
{
	// CRC covers every byte except the trailing crc32 field itself.
	return esp_rom_crc32_le(
			0,
			reinterpret_cast<const uint8_t *>(&record),
			AUTO_RECORD_SIZE - sizeof(uint32_t));
}

bool autoRecordValid(const AutoRecord &record)
{
	if (record.magic != AUTO_RECORD_MAGIC)
	{
		return false;
	}

	if (record.version != AUTO_RECORD_VERSION)
	{
		return false;
	}

	return record.crc32 == autoRecordCrc(record);
}

// True when this record's wake could not read the instantaneous snapshot.
//
// Recognized by the impossible sentinel rather than by a flag bit, because all
// eight flag bits are assigned and the 72-byte layout is frozen. Testing
// bus_uV alone is enough - the four fields are written together - but all four
// are checked so a partially-written record from some future path cannot read
// as a valid snapshot.
bool autoRecordSnapshotUnread(const AutoRecord &record)
{
	return record.bus_uV == AUTO_SNAPSHOT_BUS_UV_UNREAD &&
				 record.avg_current_uA == AUTO_SNAPSHOT_SIGNED_UNREAD &&
				 record.avg_power_uW == AUTO_SNAPSHOT_SIGNED_UNREAD &&
				 record.temp_mC == AUTO_SNAPSHOT_SIGNED_UNREAD;
}
