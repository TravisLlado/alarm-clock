#include "Alarms.h"

#include <Preferences.h>

#include "config.h"

namespace {

constexpr size_t MAX_ALARMS {NUM_BUILTIN_ALARMS + MAX_USER_ALARMS};
static_assert(NUM_BUILTIN_ALARMS <= 32, "Built-in on/off state is a 32-bit mask");

Alarms::Alarm s_alarms[MAX_ALARMS];
size_t        s_count {0};
time_t        s_lastFired[MAX_ALARMS] {};  // RAM only; 0 = not since boot

Preferences s_prefs;

// Added alarms as stored in flash
struct Stored {
  uint8_t hour;
  uint8_t minute;
  uint8_t days;
  uint8_t enabled;
};

// Fingerprint of BUILTIN_ALARMS, saved with the built-ins' on/off mask. If
// config.h's list changes, the saved mask no longer lines up with it, so it's
// discarded and every built-in starts out on.
uint32_t builtinSignature() {
  uint32_t h{2166136261U};  // FNV-1a
  auto mix = [&h](uint8_t b) { h = (h ^ b) * 16777619U; };
  mix((uint8_t)NUM_BUILTIN_ALARMS);
  for (const AlarmDef& a : BUILTIN_ALARMS) {
    mix(a.hour);
    mix(a.minute);
    mix(a.days);
  }
  return h;
}

void save() {
  uint32_t mask{0};
  Stored   stored[MAX_USER_ALARMS];
  size_t   n{0};
  for (size_t i{0}; i < s_count; i++) {
    const Alarms::Alarm& a{s_alarms[i]};
    if (a.builtin) {
      if (a.enabled) mask |= 1U << i;
    } else {
      stored[n++] = {a.hour, a.minute, a.days, (uint8_t)a.enabled};
    }
  }
  s_prefs.begin(ALARMS_NAMESPACE, false);
  s_prefs.putUInt("bi_sig", builtinSignature());
  s_prefs.putUInt("bi_mask", mask);
  if (n > 0) {
    s_prefs.putBytes("user", stored, n * sizeof(Stored));
  } else {
    s_prefs.remove("user");  // putBytes() ignores zero-length writes
  }
  s_prefs.end();
}

bool dayMatches(uint8_t days, int wday) { return days == ONCE || (days & (1U << wday)); }

const char* const DAY_NAMES[7] {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};

int dayIndex(const char* s, size_t len) {
  for (int d{0}; d < 7; d++) {
    if (len == 3 && strncmp(s, DAY_NAMES[d], 3) == 0) return d;
  }
  return -1;
}

}  // namespace

namespace Alarms {

void begin() {
  s_prefs.begin(ALARMS_NAMESPACE, true);
  const bool     maskValid{s_prefs.getUInt("bi_sig", 0) == builtinSignature()};
  const uint32_t mask{maskValid ? s_prefs.getUInt("bi_mask", 0xFFFFFFFFU) : 0xFFFFFFFFU};

  s_count = 0;
  for (size_t i{0}; i < NUM_BUILTIN_ALARMS; i++) {
    const AlarmDef& d{BUILTIN_ALARMS[i]};
    s_alarms[s_count++] = {d.hour, d.minute, d.days, (mask & (1U << i)) != 0, true};
  }

  Stored stored[MAX_USER_ALARMS];
  const size_t bytes{s_prefs.getBytesLength("user")};
  size_t n{0};
  if (bytes > 0 && bytes % sizeof(Stored) == 0 && bytes <= sizeof(stored)) {
    s_prefs.getBytes("user", stored, bytes);
    n = bytes / sizeof(Stored);
  }
  s_prefs.end();

  for (size_t i{0}; i < n; i++) {
    const Stored& st{stored[i]};
    if (st.hour > 23 || st.minute > 59 || st.days > DAILY) continue;  // corrupt
    s_alarms[s_count++] = {st.hour, st.minute, st.days, st.enabled != 0, false};
  }
}

size_t count() { return s_count; }

const Alarm& get(size_t index) { return s_alarms[index]; }

bool add(uint8_t hour, uint8_t minute, uint8_t days, String& err) {
  if (s_count >= MAX_ALARMS) {
    err = String("Alarm list is full (") + MAX_USER_ALARMS + " added alarms max).";
    return false;
  }
  s_alarms[s_count] = {hour, minute, days, true, false};
  s_lastFired[s_count] = 0;
  s_count++;
  save();
  return true;
}

bool remove(size_t index, String& err) {
  if (index >= s_count) {
    err = "No such alarm.";
    return false;
  }
  if (s_alarms[index].builtin) {
    err = "Built-in alarms (from config.h) can't be removed, only turned off.";
    return false;
  }
  for (size_t i{index}; i + 1 < s_count; i++) {
    s_alarms[i]    = s_alarms[i + 1];
    s_lastFired[i] = s_lastFired[i + 1];
  }
  s_count--;
  save();
  return true;
}

bool setEnabled(size_t index, bool enabled, String& err) {
  if (index >= s_count) {
    err = "No such alarm.";
    return false;
  }
  s_alarms[index].enabled = enabled;
  save();
  return true;
}

bool takeDue(const struct tm& lt, time_t when, size_t& first) {
  bool any{false};
  bool changed{false};
  for (size_t i{0}; i < s_count; i++) {
    Alarm& a{s_alarms[i]};
    if (!a.enabled || a.hour != lt.tm_hour || a.minute != lt.tm_min ||
        !dayMatches(a.days, lt.tm_wday)) {
      continue;
    }
    if (s_lastFired[i] != 0 && when - s_lastFired[i] < ALARM_REFIRE_GUARD) continue;
    s_lastFired[i] = when;
    if (a.days == ONCE) {
      a.enabled = false;
      changed = true;
    }
    if (!any) first = i;
    any = true;
  }
  if (changed) save();
  return any;
}

bool next(time_t now, time_t& when, size_t& index) {
  bool found{false};
  struct tm today;
  localtime_r(&now, &today);
  // Today plus the next 7 days covers every weekly alarm
  for (int d{0}; d <= 7; d++) {
    for (size_t i{0}; i < s_count; i++) {
      const Alarm& a{s_alarms[i]};
      if (!a.enabled) continue;
      struct tm t{today};
      t.tm_mday += d;
      t.tm_hour  = a.hour;
      t.tm_min   = a.minute;
      t.tm_sec   = 0;
      t.tm_isdst = -1;
      const time_t at{mktime(&t)};  // also normalizes t.tm_wday
      if (at <= now || !dayMatches(a.days, t.tm_wday)) continue;
      if (!found || at < when) {
        when  = at;
        index = i;
        found = true;
      }
    }
    if (found) return true;  // nothing on a later day can be sooner
  }
  return found;
}

String formatDays(uint8_t days) {
  if (days == ONCE)     return "once";
  if (days == DAILY)    return "daily";
  if (days == WEEKDAYS) return "weekdays";
  if (days == WEEKENDS) return "weekends";
  String s;
  for (int i{1}; i <= 7; i++) {  // Monday first
    const int d{i % 7};
    if (!(days & (1U << d))) continue;
    if (s.length()) s += ',';
    s += DAY_NAMES[d];
  }
  return s;
}

bool parseDays(const char* text, uint8_t& days) {
  if (strcmp(text, "once") == 0)  { days = ONCE;     return true; }
  if (strcmp(text, "daily") == 0 || strcmp(text, "everyday") == 0) {
    days = DAILY;
    return true;
  }
  if (strcmp(text, "weekdays") == 0) { days = WEEKDAYS; return true; }
  if (strcmp(text, "weekends") == 0) { days = WEEKENDS; return true; }

  // Comma-separated days or ranges: mon,wed,fri  mon-thu,sat  fri-mon
  uint8_t mask{0};
  const char* p{text};
  while (*p) {
    const char* comma{strchr(p, ',')};
    const size_t len{comma ? (size_t)(comma - p) : strlen(p)};
    const char* dash{(const char*)memchr(p, '-', len)};
    if (dash) {
      const int from{dayIndex(p, dash - p)};
      const int to{dayIndex(dash + 1, len - (dash + 1 - p))};
      if (from < 0 || to < 0) return false;
      for (int d{from};; d = (d + 1) % 7) {
        mask |= 1U << d;
        if (d == to) break;
      }
    } else {
      const int d{dayIndex(p, len)};
      if (d < 0) return false;
      mask |= 1U << d;
    }
    p += len;
    if (*p == ',') p++;
  }
  if (mask == 0) return false;
  days = mask;
  return true;
}

String describe(size_t index) {
  const Alarm& a{s_alarms[index]};
  char buf[8];
  snprintf(buf, sizeof(buf), "%02u:%02u", a.hour, a.minute);
  return String(buf) + " " + formatDays(a.days);
}

}  // namespace Alarms
