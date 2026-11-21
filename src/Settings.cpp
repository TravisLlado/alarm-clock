#include "Settings.h"

#include <Preferences.h>

#include "config.h"

namespace {

enum class Kind : uint8_t { NUMBER, WAVE, ONOFF };

struct Def {
  const char* name;  // console name, also the NVS key (15 chars max)
  Kind        kind;
  int32_t     def;
  int32_t     min;
  int32_t     max;
  const char* unit;
  const char* help;
};

// Same order as Settings::Id
const Def DEFS[] = {
  {"start_volume", Kind::NUMBER, DEFAULT_START_VOLUME,   0,   100,   "%",   "volume when an alarm starts"},
  {"max_volume",   Kind::NUMBER, DEFAULT_MAX_VOLUME,     0,   100,   "%",   "volume at the end of the ramp"},
  {"ramp",         Kind::NUMBER, DEFAULT_RAMP_S,         0,   3600,  "s",   "time to ramp from start to max volume"},
  {"wave",         Kind::WAVE,   DEFAULT_WAVEFORM,       0,   1,     "",    "sine or square"},
  {"freq",         Kind::NUMBER, DEFAULT_FREQ_HZ,        100, 6000,  "Hz",  "beep pitch"},
  {"beep",         Kind::NUMBER, DEFAULT_BEEP_MS,        10,  5000,  "ms",  "length of each beep"},
  {"gap",          Kind::NUMBER, DEFAULT_GAP_MS,         0,   5000,  "ms",  "silence between beeps in a burst"},
  {"beeps",        Kind::NUMBER, DEFAULT_BEEPS,          1,   20,    "",    "beeps per burst"},
  {"pause",        Kind::NUMBER, DEFAULT_PAUSE_MS,       0,   10000, "ms",  "silence after each burst"},
  {"away",         Kind::NUMBER, DEFAULT_AWAY_S,         0,   7200,  "s",   "after getting up, rings again if back in bed within this"},
  {"up_confirm",   Kind::NUMBER, DEFAULT_UP_CONFIRM_S,   0,   120,   "s",   "fewer in bed this long = got up"},
  {"back_confirm", Kind::NUMBER, DEFAULT_BACK_CONFIRM_S, 0,   300,   "s",   "back in bed this long = rings again"},
  {"max_ring",     Kind::NUMBER, DEFAULT_MAX_RING_MIN,   0,   240,   "min", "give up after this much ringing (0 = never)"},
  {"skip_empty",   Kind::ONOFF,  DEFAULT_SKIP_EMPTY,     0,   1,     "",    "don't ring if the bed is empty at alarm time"},
};
static_assert(sizeof(DEFS) / sizeof(DEFS[0]) == Settings::COUNT,
              "DEFS needs one entry per Settings::Id, in order");

int32_t     s_values[Settings::COUNT];
Preferences s_prefs;

bool parseValue(const Def& d, const char* text, int32_t& out) {
  if (d.kind == Kind::WAVE) {
    if (strcmp(text, "sine") == 0)   { out = WAVE_SINE;   return true; }
    if (strcmp(text, "square") == 0) { out = WAVE_SQUARE; return true; }
    return false;
  }
  if (d.kind == Kind::ONOFF) {
    if (strcmp(text, "on") == 0  || strcmp(text, "1") == 0) { out = 1; return true; }
    if (strcmp(text, "off") == 0 || strcmp(text, "0") == 0) { out = 0; return true; }
    return false;
  }
  char* end;
  const long v{strtol(text, &end, 10)};
  if (end == text || *end != '\0' || v < d.min || v > d.max) return false;
  out = (int32_t)v;
  return true;
}

}  // namespace

namespace Settings {

void begin() {
  s_prefs.begin(SETTINGS_NAMESPACE, true);
  for (size_t i{0}; i < COUNT; i++) {
    const Def& d{DEFS[i]};
    int32_t v{s_prefs.getInt(d.name, d.def)};
    if (v < d.min || v > d.max) v = d.def;  // guard against a corrupt value
    s_values[i] = v;
  }
  s_prefs.end();
}

int32_t get(Id id) { return s_values[id]; }

bool find(const char* name, Id& id) {
  for (size_t i{0}; i < COUNT; i++) {
    if (strcmp(name, DEFS[i].name) == 0) {
      id = (Id)i;
      return true;
    }
  }
  return false;
}

bool set(const char* name, const char* value, String& err) {
  Id id;
  if (find(name, id)) {
    const size_t i{id};
    const Def& d{DEFS[i]};
    int32_t v;
    if (!parseValue(d, value, v)) {
      if (d.kind == Kind::WAVE) {
        err = "Use sine or square.";
      } else if (d.kind == Kind::ONOFF) {
        err = "Use on or off.";
      } else {
        err = String("Use a whole number from ") + d.min + " to " + d.max + ".";
      }
      return false;
    }
    s_values[i] = v;
    s_prefs.begin(SETTINGS_NAMESPACE, false);
    s_prefs.putInt(d.name, v);
    s_prefs.end();
    return true;
  }
  err = String("Unknown setting '") + name + "'. Type 'settings' for the list.";
  return false;
}

void restoreDefaults() {
  for (size_t i{0}; i < COUNT; i++) s_values[i] = DEFS[i].def;
  s_prefs.begin(SETTINGS_NAMESPACE, false);
  s_prefs.clear();
  s_prefs.end();
}

const char* name(Id id) { return DEFS[id].name; }
const char* help(Id id) { return DEFS[id].help; }
bool isDefault(Id id) { return s_values[id] == DEFS[id].def; }

String valueText(Id id) {
  const Def& d{DEFS[id]};
  const int32_t v{s_values[id]};
  if (d.kind == Kind::WAVE)  return v == WAVE_SQUARE ? "square" : "sine";
  if (d.kind == Kind::ONOFF) return v ? "on" : "off";
  String s{v};
  if (d.unit[0]) {
    s += ' ';
    s += d.unit;
  }
  return s;
}

}  // namespace Settings
