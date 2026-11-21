#pragma once

// Settings - the adjustable alarm parameters (volume profile, sound, bed
// behavior). Defaults come from config.h; `set` from the console changes a
// value and saves it to flash (NVS), where it overrides the default until
// restoreDefaults().
//
// Loop task only.

#include <Arduino.h>

namespace Settings {

  // Keep in the same order as the table in Settings.cpp
  enum Id : uint8_t {
    START_VOLUME,
    MAX_VOLUME,
    RAMP_S,
    WAVEFORM,
    FREQ_HZ,
    BEEP_MS,
    GAP_MS,
    BEEPS,
    PAUSE_MS,
    AWAY_S,
    UP_CONFIRM_S,
    BACK_CONFIRM_S,
    MAX_RING_MIN,
    SKIP_EMPTY,
    COUNT
  };

  // Load saved values from flash (defaults for anything not saved)
  void begin();

  int32_t get(Id id);

  // Set by console name, e.g. set("freq", "880", err), and save to flash.
  // Returns false with a message in `err` for an unknown name or bad value.
  bool set(const char* name, const char* value, String& err);

  // Look up a setting by console name
  bool find(const char* name, Id& id);

  // Restore every default and clear the saved values
  void restoreDefaults();

  // For listing: console name, current value as text ("880 Hz", "sine",
  // "on"), and a one-line description
  const char* name(Id id);
  String      valueText(Id id);
  const char* help(Id id);
  bool        isDefault(Id id);

}  // namespace Settings
