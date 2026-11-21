/*
 * Alarm Clock - ESP32-C3 SuperMini + MAX98357A I2S amplifier
 *
 * Plays beep patterns at scheduled times, starting quiet and ramping up. The
 * alarm goes quiet when someone gets out of bed (read from the bed-occupancy
 * sensor over HTTP), and rings again if the bed fills back up within the
 * `away` time (10 min by default).
 *
 * People are counted, not sides: the alarm is satisfied while fewer sides
 * are occupied than when it started ringing. So if one of two people gets up
 * and the other rolls over to the empty side, the count stays at one and the
 * alarm stays quiet.
 *
 * Alarms come from BUILTIN_ALARMS in config.h plus any added from the web
 * console (http://<MDNS_HOSTNAME>.local/) or USB serial. Volume profile,
 * sound and bed behavior are adjustable from the console too; every change is
 * saved to flash. Type `help` for commands.
 *
 * Time comes from NTP; the timezone is looked up from the public IP's
 * location at boot, falling back to TIMEZONE_POSIX in config.h.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <WebConsole.h>
#include <stdarg.h>
#include <time.h>
#include "config.h"
#include "credentials.h"
#include "Alarms.h"
#include "Audio.h"
#include "Settings.h"
#include "WebInfo.h"

// HTTP server on configured port (URL: http://<MDNS_HOSTNAME>.local/)
AsyncWebServer server(SERVER_PORT);

// Browser console at http://<MDNS_HOSTNAME>.local/
static WebConsole webConsole;

// The console's line handler runs on the AsyncTCP task. It only copies the
// line here; loop() runs it, so all state changes, flash writes and output
// happen on one task.
static portMUX_TYPE s_cmdMux = portMUX_INITIALIZER_UNLOCKED;
static char         s_pendingCmd[CMD_MAX_LEN + 1];
static bool         s_hasPendingCmd{false};

// Timezone in use, and where it came from (for `status`)
static const char* g_tzPosix{TIMEZONE_POSIX};
static String      g_tzName{"default (config.h)"};

// ============================================================================
// Output (USB serial + web console)
// ============================================================================

static void out(const char* fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.print(buf);
  webConsole.print(buf);
}

// Local time as "Sat 21 Nov 07:00"
static String formatWhen(time_t t) {
  struct tm lt;
  localtime_r(&t, &lt);
  char buf[24];
  strftime(buf, sizeof(buf), "%a %d %b %H:%M", &lt);
  return String(buf);
}

// Duration as "13h 02m" or "4m 10s"
static String formatDuration(int32_t s) {
  if (s < 0) s = 0;
  char buf[24];
  if (s >= HOUR) {
    snprintf(buf, sizeof(buf), "%ldh %02ldm", (long)(s / HOUR), (long)(s % HOUR / MINUTE));
  } else {
    snprintf(buf, sizeof(buf), "%ldm %02lds", (long)(s / MINUTE), (long)(s % MINUTE));
  }
  return String(buf);
}

// ============================================================================
// Alarm session
// ============================================================================
//
// IDLE -> RINGING when an alarm goes off (or `ring`).
// RINGING -> AWAY once fewer people read in bed than `baseline` for
//   up_confirm seconds. The sound stops.
// AWAY -> RINGING if the bed is back to `baseline` people for back_confirm
//   seconds within `away` seconds of getting up. The volume ramp carries on
//   from where it was.
// AWAY -> IDLE once `away` seconds pass without that. Done for this alarm.
// RINGING -> IDLE after max_ring minutes of ringing in total, or `stop`.
//
// `baseline` is the most people confirmed in bed while ringing: the first
// reading when the alarm starts, raised if more people read for
// back_confirm seconds. It's treated as at least 1, so if the sensor was
// down at the start and first reads empty, that counts as everyone up.

enum class State : uint8_t { IDLE, RINGING, AWAY };

struct Session {
  State    state{State::IDLE};
  int      baseline{0};
  int      lastCount{-1};          // latest reading, -1 = none or failed
  uint32_t ringMs{0};              // ringing time before the current stretch
  uint32_t stretchStartMs{0};      // when the current ringing stretch started
  uint32_t awayStartMs{0};         // when they got up
  bool     condHolding{false};     // pending RINGING<->AWAY condition holds...
  uint32_t condSinceMs{0};         // ...since then
  bool     moreHolding{false};     // more people than baseline read...
  uint32_t moreSinceMs{0};         // ...since then
  uint32_t lastPollMs{0};
};

static Session g_session;

// `test` playback (outside an alarm session)
static bool     g_testing{false};
static uint32_t g_testUntilMs{0};

static const char* stateName(State s) {
  switch (s) {
    case State::IDLE:    return "idle";
    case State::RINGING: return "RINGING";
    case State::AWAY:    return "out of bed";
  }
  return "?";
}

static Audio::Pattern currentPattern() {
  Audio::Pattern p;
  p.wave    = (Waveform)Settings::get(Settings::WAVEFORM);
  p.freqHz  = (uint16_t)Settings::get(Settings::FREQ_HZ);
  p.beepMs  = (uint16_t)Settings::get(Settings::BEEP_MS);
  p.gapMs   = (uint16_t)Settings::get(Settings::GAP_MS);
  p.beeps   = (uint8_t)Settings::get(Settings::BEEPS);
  p.pauseMs = (uint16_t)Settings::get(Settings::PAUSE_MS);
  return p;
}

static uint32_t totalRingMs(uint32_t now) {
  const Session& s{g_session};
  return s.ringMs + (s.state == State::RINGING ? now - s.stretchStartMs : 0);
}

// Volume profile: linear from start_volume to max_volume over `ramp` seconds
// of ringing, then held at max_volume
static float rampVolume(uint32_t ringingMs) {
  const float start{(float)Settings::get(Settings::START_VOLUME)};
  const float max{(float)Settings::get(Settings::MAX_VOLUME)};
  const float rampMs{Settings::get(Settings::RAMP_S) * 1000.0F};
  if (rampMs <= 0.0F || ringingMs >= rampMs) return max;
  return start + (max - start) * (ringingMs / rampMs);
}

// A fresh bed reading, or false if the sensor can't be reached or its
// reading is stale
static bool readBed(int& count) {
  WebInfo::BedState bed;
  if (!WebInfo::fetchBedState(BED_OCCUPANCY_URL, BED_HTTP_TIMEOUT_MS, bed)) return false;
  const int64_t age{(int64_t)time(nullptr) - bed.time};
  if (age > BED_MAX_READING_AGE || age < -BED_MAX_READING_AGE) return false;
  count = bed.count;
  return true;
}

static void endSession(const char* why) {
  Audio::stop();
  g_session.state = State::IDLE;
  out("Alarm off: %s\n", why);
}

static void startSession(const char* why) {
  Session& s{g_session};
  if (s.state != State::IDLE) {
    out("%s -- alarm already going, ignored.\n", why);
    return;
  }
  g_testing = false;

  int count;
  const bool known{readBed(count)};
  if (known && count == 0 && Settings::get(Settings::SKIP_EMPTY)) {
    out("%s -- bed is empty, not ringing.\n", why);
    return;
  }

  const uint32_t now{millis()};
  s = Session{};
  s.state          = State::RINGING;
  s.baseline       = known ? count : 0;
  s.lastCount      = known ? count : -1;
  s.stretchStartMs = now;
  s.lastPollMs     = now;
  Audio::play(currentPattern());
  Audio::setVolume(rampVolume(0));

  if (known) {
    out("%s -- ringing. %d in bed; quiet once anyone gets up.\n", why, count);
  } else {
    out("%s -- ringing. Bed sensor not answering; will keep trying.\n", why);
  }
}

// One bed reading, while RINGING or AWAY
static void onBedReading(int count, uint32_t now) {
  Session& s{g_session};
  const uint32_t upConfirmMs{(uint32_t)Settings::get(Settings::UP_CONFIRM_S) * 1000U};
  const uint32_t backConfirmMs{(uint32_t)Settings::get(Settings::BACK_CONFIRM_S) * 1000U};

  // Sensor was down when the alarm started: the first occupied reading
  // sets the baseline
  if (s.baseline == 0 && count > 0) s.baseline = count;
  s.lastCount = count;

  if (s.state == State::RINGING) {
    // Someone else got in: raise the baseline, once it's clearly not a
    // rollover pressing both sides for a moment
    if (count > s.baseline) {
      if (!s.moreHolding) {
        s.moreHolding = true;
        s.moreSinceMs = now;
      }
      if (now - s.moreSinceMs >= backConfirmMs) {
        s.baseline = count;
        s.moreHolding = false;
        out("%d in bed now.\n", count);
      }
    } else {
      s.moreHolding = false;
    }

    const int needed{s.baseline > 0 ? s.baseline : 1};
    if (count < needed) {
      if (!s.condHolding) {
        s.condHolding = true;
        s.condSinceMs = now;
      }
      if (now - s.condSinceMs >= upConfirmMs) {
        s.ringMs += now - s.stretchStartMs;
        s.state = State::AWAY;
        s.awayStartMs = now;
        s.condHolding = false;
        s.moreHolding = false;
        Audio::stop();
        out("Out of bed (%d -> %d). Quiet; rings again if back in bed within %s.\n",
            needed, count, formatDuration(Settings::get(Settings::AWAY_S)).c_str());
      }
    } else {
      s.condHolding = false;
    }
  } else if (s.state == State::AWAY) {
    const int needed{s.baseline > 0 ? s.baseline : 1};
    if (count >= needed) {
      if (!s.condHolding) {
        s.condHolding = true;
        s.condSinceMs = now;
      }
      if (now - s.condSinceMs >= backConfirmMs) {
        s.state = State::RINGING;
        s.stretchStartMs = now;
        s.condHolding = false;
        Audio::play(currentPattern());
        Audio::setVolume(rampVolume(totalRingMs(now)));
        out("Back in bed (%d) -- ringing again.\n", count);
      }
    } else {
      s.condHolding = false;
    }
  }
}

static void serviceSession() {
  Session& s{g_session};
  const uint32_t now{millis()};

  if (g_testing && (int32_t)(now - g_testUntilMs) >= 0) {
    g_testing = false;
    if (s.state != State::RINGING) Audio::stop();
  }

  if (s.state == State::IDLE) return;

  if (s.state == State::RINGING) {
    const uint32_t rung{totalRingMs(now)};
    const int32_t maxRingMin{Settings::get(Settings::MAX_RING_MIN)};
    if (maxRingMin > 0 && rung >= (uint32_t)maxRingMin * 60000U) {
      char why[48];
      snprintf(why, sizeof(why), "rang for %ld min, giving up.", (long)maxRingMin);
      endSession(why);
      return;
    }
    // Picks up setting changes made while ringing
    Audio::play(currentPattern());
    Audio::setVolume(rampVolume(rung));
  } else if (s.state == State::AWAY) {
    if (now - s.awayStartMs >= (uint32_t)Settings::get(Settings::AWAY_S) * 1000U) {
      endSession("stayed out of bed. Good morning!");
      return;
    }
  }

  if (now - s.lastPollMs >= BED_POLL_INTERVAL_MS) {
    s.lastPollMs = now;
    int count;
    if (readBed(count)) {
      // Time each reading by when its poll started, not when the HTTP reply
      // arrived. Polls start BED_POLL_INTERVAL_MS apart, so with up_confirm
      // of 1 s the second "fewer" reading always confirms, however long each
      // request took.
      onBedReading(count, now);
    } else {
      s.lastCount = -1;
    }
  }
}

// ============================================================================
// Schedule
// ============================================================================

// Last minute checked for due alarms, Unix seconds (a multiple of 60)
static time_t g_lastAlarmCheck{0};

// Check each minute since the last check (up to ALARM_CATCHUP back) for due
// alarms. Steps through UTC minutes and converts each, so DST changes are
// handled by the TZ rules.
static void checkAlarms() {
  const time_t nowMin{time(nullptr) / 60 * 60};
  if (nowMin <= g_lastAlarmCheck) {
    // A big backward clock step: start over from now
    if (g_lastAlarmCheck - nowMin > ALARM_CATCHUP) g_lastAlarmCheck = nowMin;
    return;
  }

  time_t t{g_lastAlarmCheck + 60};
  if (t < nowMin - ALARM_CATCHUP) t = nowMin - ALARM_CATCHUP;
  for (; t <= nowMin; t += 60) {
    struct tm lt;
    localtime_r(&t, &lt);
    size_t idx;
    if (Alarms::takeDue(lt, t, idx)) {
      char why[48];
      snprintf(why, sizeof(why), "Alarm %u (%s)", (unsigned)(idx + 1),
               Alarms::describe(idx).c_str());
      startSession(why);
    }
  }
  g_lastAlarmCheck = nowMin;
}

// ============================================================================
// Status
// ============================================================================

static String nextAlarmText() {
  time_t when;
  size_t idx;
  const time_t now{time(nullptr)};
  if (!Alarms::next(now, when, idx)) return "no alarms on";
  return "next " + formatWhen(when) + " (in " + formatDuration((int32_t)(when - now)) + ")";
}

static void printStatusLine() {
  const Session& s{g_session};
  const uint32_t now{millis()};
  const time_t t{time(nullptr)};
  struct tm lt;
  localtime_r(&t, &lt);
  char clock[12];
  strftime(clock, sizeof(clock), "%H:%M:%S", &lt);

  String msg{clock};
  msg += " | ";
  msg += stateName(s.state);
  if (s.state == State::RINGING) {
    char buf[48];
    snprintf(buf, sizeof(buf), " vol %.0f%% (%lus)", rampVolume(totalRingMs(now)),
             (unsigned long)(totalRingMs(now) / 1000));
    msg += buf;
  } else if (s.state == State::AWAY) {
    const int32_t left{Settings::get(Settings::AWAY_S) - (int32_t)((now - s.awayStartMs) / 1000)};
    msg += " (" + formatDuration(left) + " left)";
  }
  if (s.state != State::IDLE) {
    msg += " | bed ";
    msg += s.lastCount < 0 ? String("?") : String(s.lastCount);
    msg += "/";
    msg += s.baseline;
  }
  if (g_testing) msg += " | test";
  msg += " | ";
  msg += nextAlarmText();
  out("%s\n", msg.c_str());
}

static void printAlarms() {
  if (Alarms::count() == 0) {
    out("No alarms. Add one with: alarm add <HH:MM> [days]\n");
    return;
  }
  for (size_t i{0}; i < Alarms::count(); i++) {
    const Alarms::Alarm& a{Alarms::get(i)};
    out("%2u  %-24s %-3s %s\n", (unsigned)(i + 1), Alarms::describe(i).c_str(),
        a.enabled ? "on" : "off", a.builtin ? "(config.h)" : "");
  }
  out("%s\n", nextAlarmText().c_str());
}

static void printSettings() {
  for (size_t i{0}; i < Settings::COUNT; i++) {
    const Settings::Id id{(Settings::Id)i};
    out("  %-12s %-8s %s%s\n", Settings::name(id), Settings::valueText(id).c_str(),
        Settings::help(id), Settings::isDefault(id) ? "" : " *");
  }
  out("  (* = changed from config.h default)\n");
}

static void printHelp() {
  out("Commands:\n");
  out("  status                         time, alarm state, next alarm\n");
  out("  alarms                         list alarms\n");
  out("  alarm add <HH:MM> [days]       days: daily (default), weekdays, weekends,\n");
  out("                                 once, or a list like mon,wed,fri / mon-thu\n");
  out("  alarm del <n>                  remove an added alarm\n");
  out("  alarm on|off <n>               turn an alarm on or off\n");
  out("  settings                       list volume, sound and bed settings\n");
  out("  set <name> <value>             change a setting (saved), e.g. set ramp 90\n");
  out("  defaults                       restore every setting to its config.h default\n");
  out("  ring                           start an alarm now\n");
  out("  test [volume] [seconds]        play the sound at a fixed volume\n");
  out("  stop                           stop the alarm or test\n");
}

// ============================================================================
// Console commands
// ============================================================================

// "7:05" / "07:05" -> hour, minute
static bool parseTime(const char* s, uint8_t& hour, uint8_t& minute) {
  unsigned h, m;
  char extra;
  if (sscanf(s, "%u:%u%c", &h, &m, &extra) != 2 || h > 23 || m > 59) return false;
  hour = (uint8_t)h;
  minute = (uint8_t)m;
  return true;
}

// Console alarm number (1-based) -> index, or -1
static int parseAlarmNumber(const char* s) {
  char* end;
  const long n{strtol(s, &end, 10)};
  if (end == s || *end != '\0' || n < 1 || n > (long)Alarms::count()) return -1;
  return (int)(n - 1);
}

static void cmdAlarm(char** tok, size_t count) {
  String err;
  if (count >= 3 && strcmp(tok[1], "add") == 0) {
    uint8_t hour, minute;
    uint8_t days{DAILY};
    if (!parseTime(tok[2], hour, minute)) {
      out("Bad time '%s'. Use 24-hour HH:MM, e.g. 06:45.\n", tok[2]);
      return;
    }
    if (count >= 4 && !Alarms::parseDays(tok[3], days)) {
      out("Bad days '%s'. Use daily, weekdays, weekends, once, or e.g. mon,wed,fri.\n", tok[3]);
      return;
    }
    if (!Alarms::add(hour, minute, days, err)) {
      out("%s\n", err.c_str());
      return;
    }
    out("Added alarm %u: %s\n", (unsigned)Alarms::count(),
        Alarms::describe(Alarms::count() - 1).c_str());
    out("%s\n", nextAlarmText().c_str());
    return;
  }

  if (count == 3 && (strcmp(tok[1], "del") == 0 || strcmp(tok[1], "on") == 0 ||
                     strcmp(tok[1], "off") == 0)) {
    const int idx{parseAlarmNumber(tok[2])};
    if (idx < 0) {
      out("No alarm %s. Type 'alarms' for the list.\n", tok[2]);
      return;
    }
    const String desc{Alarms::describe(idx)};
    bool ok;
    if (strcmp(tok[1], "del") == 0) {
      ok = Alarms::remove(idx, err);
      if (ok) out("Removed %s\n", desc.c_str());
    } else {
      const bool on{strcmp(tok[1], "on") == 0};
      ok = Alarms::setEnabled(idx, on, err);
      if (ok) out("Alarm %d (%s) %s\n", idx + 1, desc.c_str(), on ? "on" : "off");
    }
    if (!ok) out("%s\n", err.c_str());
    return;
  }

  out("Usage: alarm add <HH:MM> [days] | alarm del <n> | alarm on|off <n>\n");
}

static void cmdTest(char** tok, size_t count) {
  if (g_session.state != State::IDLE) {
    out("An alarm is going -- 'stop' it first.\n");
    return;
  }
  long volume{Settings::get(Settings::MAX_VOLUME)};
  long seconds{TEST_DEFAULT_SECONDS};
  char* end;
  if (count >= 2) {
    volume = strtol(tok[1], &end, 10);
    if (*end != '\0' || volume < 0 || volume > 100) {
      out("Volume is 0-100.\n");
      return;
    }
  }
  if (count >= 3) {
    seconds = strtol(tok[2], &end, 10);
    if (*end != '\0' || seconds < 1 || seconds > TEST_MAX_SECONDS) {
      out("Seconds is 1-%ld.\n", (long)TEST_MAX_SECONDS);
      return;
    }
  }
  Audio::play(currentPattern());
  Audio::setVolume((float)volume);
  g_testing = true;
  g_testUntilMs = millis() + (uint32_t)seconds * 1000U;
  out("Test: volume %ld for %ld s\n", volume, seconds);
}

static void handleCommand(char* line) {
  for (char* p{line}; *p; p++) *p = tolower((unsigned char)*p);

  char* tok[5];
  size_t count{0};
  for (char* t{strtok(line, " \t\r\n")}; t && count < 5; t = strtok(nullptr, " \t\r\n")) {
    tok[count++] = t;
  }
  if (count == 0) return;
  const char* cmd{tok[0]};

  if (strcmp(cmd, "status") == 0) {
    out("Timezone: %s (%s)\n", g_tzName.c_str(), g_tzPosix);
    printStatusLine();
  } else if (strcmp(cmd, "alarms") == 0) {
    printAlarms();
  } else if (strcmp(cmd, "alarm") == 0) {
    cmdAlarm(tok, count);
  } else if (strcmp(cmd, "settings") == 0) {
    printSettings();
  } else if (strcmp(cmd, "set") == 0) {
    if (count != 3) {
      out("Usage: set <name> <value>. Type 'settings' for names.\n");
      return;
    }
    String err;
    Settings::Id id;
    if (Settings::set(tok[1], tok[2], err) && Settings::find(tok[1], id)) {
      out("%s = %s (saved)\n", tok[1], Settings::valueText(id).c_str());
    } else {
      out("%s\n", err.c_str());
    }
  } else if (strcmp(cmd, "defaults") == 0) {
    Settings::restoreDefaults();
    out("Settings restored to config.h defaults.\n");
    printSettings();
  } else if (strcmp(cmd, "ring") == 0) {
    startSession("Manual alarm");
  } else if (strcmp(cmd, "test") == 0) {
    cmdTest(tok, count);
  } else if (strcmp(cmd, "stop") == 0) {
    if (g_session.state != State::IDLE) {
      endSession("stopped from the console.");
    } else if (g_testing) {
      g_testing = false;
      Audio::stop();
      out("Test stopped.\n");
    } else {
      out("Nothing playing.\n");
    }
  } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
    printHelp();
  } else {
    out("Unknown command '%s'. Type 'help' for commands.\n", cmd);
  }
}

// AsyncTCP task: queue the line for loop(). Dropped if the previous one
// hasn't run yet or it's too long to be a command.
static void onConsoleLine(const char* line, size_t len) {
  if (len > CMD_MAX_LEN) return;
  portENTER_CRITICAL(&s_cmdMux);
  if (!s_hasPendingCmd) {
    memcpy(s_pendingCmd, line, len);
    s_pendingCmd[len] = '\0';
    s_hasPendingCmd = true;
  }
  portEXIT_CRITICAL(&s_cmdMux);
}

// Loop task: run the queued web console command, if any
static void pollWebCommand() {
  char line[CMD_MAX_LEN + 1];
  bool have{false};
  portENTER_CRITICAL(&s_cmdMux);
  if (s_hasPendingCmd) {
    memcpy(line, s_pendingCmd, sizeof(line));
    s_hasPendingCmd = false;
    have = true;
  }
  portEXIT_CRITICAL(&s_cmdMux);
  if (have) handleCommand(line);
}

// Accumulate USB serial characters and run each complete line
static void pollSerialCommand() {
  static char   buf[CMD_MAX_LEN + 1];
  static size_t len{0};
  while (Serial.available()) {
    const char c{(char)Serial.read()};
    if (c == '\n' || c == '\r') {
      if (len > 0) {
        buf[len] = '\0';
        handleCommand(buf);
        len = 0;
      }
    } else if (len < CMD_MAX_LEN) {
      buf[len++] = c;
    }
  }
}

// ============================================================================
// Setup
// ============================================================================

// Pick the timezone from the public IP's location. Falls back to
// TIMEZONE_POSIX if the lookup fails or the zone isn't in WebInfo's table.
static void lookupTimezone() {
  float  lat, lon;
  String zone;
  for (uint8_t attempt{0}; attempt < GEO_ATTEMPTS; attempt++) {
    if (WebInfo::fetchGeo(lat, lon, zone)) {
      Serial.printf("Geo approx: %.*f, %.*f, %s\n", GEO_COORD_DECIMALS, lat,
                    GEO_COORD_DECIMALS, lon, zone.c_str());
      const char* posix{WebInfo::posixTzFor(zone)};
      if (posix) {
        g_tzPosix = posix;
        g_tzName  = zone;
      } else {
        Serial.printf("No TZ rules for %s, using default.\n", zone.c_str());
        g_tzName = zone + " unknown, default (config.h)";
      }
      return;
    }
    Serial.println("Geo lookup failed, retrying...");
    delay(RETRY_DELAY_MS);
  }
  Serial.println("Geo lookup failed, using default timezone.");
}

void setup() {
  Serial.begin(SERIAL_BAUD_RATE);
  Serial.println("\r\n\r\nAlarm Clock");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_OFF);

  Settings::begin();
  Alarms::begin();
  Audio::begin();

  // Connect to WiFi -- retry indefinitely; there's no RTC, so nothing works
  // until the clock is set from the network
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(MDNS_HOSTNAME);
  // Give the station an IPv6 link-local address once it joins, so mDNS can
  // answer AAAA queries. Without one, macOS waits ~5 s on every lookup.
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t) { WiFi.enableIpV6(); },
               ARDUINO_EVENT_WIFI_STA_CONNECTED);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("WiFi failed, retrying in 5s...");
    delay(RETRY_DELAY_MS);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", SERVER_PORT);
    Serial.println(String("mDNS: http://") + MDNS_HOSTNAME + ".local/");
  } else {
    Serial.println("mDNS failed");
  }
  Serial.println("Console: http://" + WiFi.localIP().toString() + "/");

  // Timezone from location, then the clock from NTP. The first sync is
  // required; retry until it succeeds.
  lookupTimezone();
  WebInfo::initTime(g_tzPosix);
  while (!WebInfo::clockValid() && !WebInfo::syncClock(NTP_SYNC_TIMEOUT_MS)) {
    Serial.println("Time sync failed, retrying in 5s...");
    delay(RETRY_DELAY_MS);
  }

  // Check alarms from the current minute on, so an alarm set for the minute
  // we booted in still fires
  g_lastAlarmCheck = time(nullptr) / 60 * 60 - 60;

  webConsole.begin(server, "/", onConsoleLine);
  server.begin();

  const time_t now{time(nullptr)};
  out("Clock set: %s, timezone %s\n", formatWhen(now).c_str(), g_tzName.c_str());
  out("Console: http://%s.local/  (http://%s/)\n", MDNS_HOSTNAME,
      WiFi.localIP().toString().c_str());
  printAlarms();
  out("Type 'help' for commands.\n");
}

// ============================================================================
// Main loop
// ============================================================================

void loop() {
  static uint32_t lastWifiCheckMs{0};
  static uint32_t lastStatusMs{0};
  static int      lastRefreshDay{-1};

  const uint32_t now{millis()};

  pollSerialCommand();
  pollWebCommand();

  // Periodic WiFi watchdog: re-join if the connection has dropped. Alarms
  // keep working offline; only the bed sensor and console need WiFi.
  if (now - lastWifiCheckMs >= WIFI_CHECK_INTERVAL_MS) {
    lastWifiCheckMs = now;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi lost, reconnecting...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }

  // Daily NTP re-sync to correct drift. Never while an alarm is going (it can
  // block for up to NTP_SYNC_TIMEOUT_MS). A failure isn't fatal: the clock
  // keeps running from its last sync, and the TZ rules keep DST correct.
  {
    const time_t t{time(nullptr)};
    struct tm lt;
    localtime_r(&t, &lt);
    if (lastRefreshDay < 0) lastRefreshDay = lt.tm_yday;  // just synced at boot
    if (lt.tm_hour >= DAILY_REFRESH_HOUR && lt.tm_yday != lastRefreshDay &&
        g_session.state == State::IDLE) {
      lastRefreshDay = lt.tm_yday;
      out("%s\n", WebInfo::syncClock(NTP_SYNC_TIMEOUT_MS)
                      ? "Clock re-synced from NTP."
                      : "NTP sync failed, using onboard clock.");
    }
  }

  checkAlarms();
  serviceSession();

  // LED: on while ringing or testing, blinking while out of bed
  const State st{g_session.state};
  const bool led{st == State::RINGING || g_testing ||
                 (st == State::AWAY && (now / LED_BLINK_MS) % 2 == 0)};
  digitalWrite(LED_PIN, led ? LED_ON : LED_OFF);

  const uint32_t statusInterval{st == State::IDLE ? STATUS_IDLE_INTERVAL_MS
                                                  : STATUS_ACTIVE_INTERVAL_MS};
  if (now - lastStatusMs >= statusInterval) {
    lastStatusMs = now;
    printStatusLine();
  }

  // Send this tick's console output to the browser
  webConsole.loop();

  delay(TICK_PERIOD_MS);
}

// End of file /////////////////////////////////////////////////////////////////
