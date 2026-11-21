#include "WebInfo.h"

#include <HTTPClient.h>
#include <esp_sntp.h>
#include <time.h>

#include "config.h"

// Incremented by the SNTP task each time the clock is set from NTP.
static volatile uint32_t s_syncCount{0};

static void onTimeSync(struct timeval*) { s_syncCount = s_syncCount + 1; }

// IANA timezone -> POSIX TZ rules. Add entries as needed; anything missing
// falls back to TIMEZONE_POSIX in config.h.
struct TzEntry {
  const char* iana;
  const char* posix;
};
static const TzEntry TZ_TABLE[] = {
  {"America/Los_Angeles",           "PST8PDT,M3.2.0,M11.1.0"},
  {"America/Vancouver",             "PST8PDT,M3.2.0,M11.1.0"},
  {"America/Denver",                "MST7MDT,M3.2.0,M11.1.0"},
  {"America/Boise",                 "MST7MDT,M3.2.0,M11.1.0"},
  {"America/Edmonton",              "MST7MDT,M3.2.0,M11.1.0"},
  {"America/Phoenix",               "MST7"},
  {"America/Chicago",               "CST6CDT,M3.2.0,M11.1.0"},
  {"America/Winnipeg",              "CST6CDT,M3.2.0,M11.1.0"},
  {"America/New_York",              "EST5EDT,M3.2.0,M11.1.0"},
  {"America/Detroit",               "EST5EDT,M3.2.0,M11.1.0"},
  {"America/Toronto",               "EST5EDT,M3.2.0,M11.1.0"},
  {"America/Indiana/Indianapolis",  "EST5EDT,M3.2.0,M11.1.0"},
  {"America/Kentucky/Louisville",   "EST5EDT,M3.2.0,M11.1.0"},
  {"America/Halifax",               "AST4ADT,M3.2.0,M11.1.0"},
  {"America/Anchorage",             "AKST9AKDT,M3.2.0,M11.1.0"},
  {"Pacific/Honolulu",              "HST10"},
  {"Europe/London",                 "GMT0BST,M3.5.0/1,M10.5.0"},
  {"Europe/Dublin",                 "IST-1GMT0,M10.5.0,M3.5.0/1"},
  {"Europe/Paris",                  "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Berlin",                 "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Madrid",                 "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Rome",                   "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Amsterdam",              "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Asia/Tokyo",                    "JST-9"},
  {"Australia/Sydney",              "AEST-10AEDT,M10.1.0,M4.1.0/3"},
};

namespace WebInfo {

bool fetchGeo(float& lat, float& lon, String& timezone) {
  HTTPClient http;
  if (!http.begin("http://ip-api.com/line/?fields=status,lat,lon,timezone")) return false;

  const int code{http.GET()};
  if (code != 200) { http.end(); return false; }

  String body{http.getString()};
  http.end();

  // Response is four lines: status, lat, lon, timezone
  String lines[4];
  int start{0};
  for (int i{0}; i < 4; i++) {
    int nl{body.indexOf('\n', start)};
    if (nl < 0) nl = body.length();
    lines[i] = body.substring(start, nl);
    lines[i].trim();
    start = nl + 1;
  }
  if (lines[0] != "success" || lines[1].length() == 0 || lines[2].length() == 0) return false;

  lat = lines[1].toFloat();
  lon = lines[2].toFloat();
  timezone = lines[3];
  return true;
}

const char* posixTzFor(const String& ianaName) {
  for (const TzEntry& e : TZ_TABLE) {
    if (ianaName == e.iana) return e.posix;
  }
  return nullptr;
}

// Raw text of a top-level JSON field's value (number/bool/null), e.g. "true"
// or "1763512345". Enough for the sensor's flat, fixed-format object.
static bool jsonValue(const String& body, const char* key, String& out) {
  const String pattern{String("\"") + key + "\":"};
  const int start{body.indexOf(pattern)};
  if (start < 0) return false;
  int i{start + static_cast<int>(pattern.length())};
  int end{i};
  while (end < static_cast<int>(body.length()) && body[end] != ',' && body[end] != '}') end++;
  out = body.substring(i, end);
  out.trim();
  return out.length() > 0;
}

static bool jsonBool(const String& body, const char* key, bool& out) {
  String v;
  if (!jsonValue(body, key, v)) return false;
  if (v == "true")  { out = true;  return true; }
  if (v == "false") { out = false; return true; }
  return false;
}

static bool jsonInt64(const String& body, const char* key, int64_t& out) {
  String v;
  if (!jsonValue(body, key, v)) return false;
  char* endPtr{nullptr};
  const long long n{strtoll(v.c_str(), &endPtr, 10)};
  if (endPtr == v.c_str() || *endPtr != '\0') return false;  // also rejects null
  out = n;
  return true;
}

bool fetchBedState(const char* url, uint32_t timeoutMs, BedState& out) {
  HTTPClient http;
  http.setConnectTimeout(timeoutMs);
  http.setTimeout(timeoutMs);
  if (!http.begin(url)) return false;

  const int code{http.GET()};
  if (code != 200) { http.end(); return false; }

  String body{http.getString()};
  http.end();

  bool left, right;
  BedState s;
  if (!jsonBool(body, "left", left) || !jsonBool(body, "right", right) ||
      !jsonInt64(body, "time", s.time)) {
    return false;
  }
  s.count = (uint8_t)left + (uint8_t)right;
  out = s;
  return true;
}

void initTime(const char* posixTz) {
  sntp_set_time_sync_notification_cb(onTimeSync);
  configTzTime(posixTz, NTP_SERVER_1, NTP_SERVER_2);
}

bool syncClock(uint32_t timeoutMs) {
  const uint32_t before{s_syncCount};
  sntp_restart();  // send a request now rather than at the next poll interval
  const uint32_t start{millis()};
  while (millis() - start < timeoutMs) {
    if (s_syncCount != before) return true;
    delay(100);
  }
  return false;
}

bool clockValid() { return s_syncCount > 0; }

}  // namespace WebInfo
