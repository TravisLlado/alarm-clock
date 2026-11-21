#pragma once

#include <Arduino.h>

namespace WebInfo {

  // One reading from the bed-occupancy sensor's GET /state endpoint.
  struct BedState {
    uint8_t count;  // occupied sides of the bed, 0-2 (= people in bed)
    int64_t time;   // when the reading was taken, Unix seconds UTC
  };

  // GET the sensor's /state JSON. Returns false on any network/HTTP error,
  // a malformed body, or a null `time` (sensor clock not yet set).
  bool fetchBedState(const char* url, uint32_t timeoutMs, BedState& out);

  // Fetch approximate location and IANA timezone name (e.g.
  // "America/Los_Angeles") from the device's public IP (ip-api.com).
  bool fetchGeo(float& lat, float& lon, String& timezone);

  // POSIX TZ string (with DST rules) for an IANA timezone name, or nullptr
  // if it isn't in the table.
  const char* posixTzFor(const String& ianaName);

  // Start SNTP and set the local timezone (POSIX TZ string, DST rules
  // included). Call once at boot. The ESP32 keeps re-syncing in the
  // background after this.
  void initTime(const char* posixTz);

  // Request an NTP sync now and wait up to timeoutMs for it to complete.
  // Returns true if the clock was updated from NTP. On failure the clock keeps
  // running from its last sync, and local time/DST stay correct.
  bool syncClock(uint32_t timeoutMs);

  // True once the clock has been set from NTP at least once since boot.
  bool clockValid();

}  // namespace WebInfo
