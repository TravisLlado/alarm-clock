#pragma once

// Alarms - the alarm list and its schedule
//
// One list, numbered from 0 here (from 1 on the console): the built-in alarms
// from BUILTIN_ALARMS in config.h first, then any added from the console.
// Built-ins can be turned off and on but not removed; added alarms can also
// be removed. Every change is saved to flash.
//
// Loop task only.

#include <Arduino.h>
#include <time.h>

namespace Alarms {

  struct Alarm {
    uint8_t hour;     // 0-23 local
    uint8_t minute;   // 0-59
    uint8_t days;     // day mask (see config.h); ONCE = next occurrence only
    bool    enabled;
    bool    builtin;  // from config.h
  };

  // Load the added alarms and the built-ins' on/off state from flash
  void begin();

  size_t       count();
  const Alarm& get(size_t index);

  // Add an alarm. Returns false (with a message in `err`) if the list is full.
  bool add(uint8_t hour, uint8_t minute, uint8_t days, String& err);
  bool remove(size_t index, String& err);
  bool setEnabled(size_t index, bool enabled, String& err);

  // Is any enabled alarm due at local time `lt` (= `when`, to the minute)?
  // Marks each due alarm as fired (ONCE alarms turn themselves off) and
  // returns the first one's index in `first`. An alarm that fired less than
  // ALARM_REFIRE_GUARD ago is skipped.
  bool takeDue(const struct tm& lt, time_t when, size_t& first);

  // The next time any enabled alarm will go off after `now`
  bool next(time_t now, time_t& when, size_t& index);

  // Day mask <-> text: "daily", "weekdays", "weekends", "once", or a list
  // like "mon,wed,fri" / "mon-thu,sat". parseDays expects lowercase.
  String formatDays(uint8_t days);
  bool   parseDays(const char* text, uint8_t& days);

  // "07:00 weekdays"
  String describe(size_t index);

}  // namespace Alarms
