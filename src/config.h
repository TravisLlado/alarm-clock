// config.h ////////////////////////////////////////////////////////////////////

#pragma once

#include <Arduino.h>

// Time units (seconds) ////////////////////////////////////////////////////////

constexpr int32_t SECOND{1};
constexpr int32_t MINUTE{60 * SECOND};
constexpr int32_t HOUR{60 * MINUTE};

// Alarms //////////////////////////////////////////////////////////////////////

// Day masks, one bit per day of the week (bit 0 = Sunday, matching tm_wday).
// Combine with |, e.g. MON | WED | FRI. ONCE fires at the next occurrence of
// its time and then turns itself off.
constexpr uint8_t SUN {1U << 0};
constexpr uint8_t MON {1U << 1};
constexpr uint8_t TUE {1U << 2};
constexpr uint8_t WED {1U << 3};
constexpr uint8_t THU {1U << 4};
constexpr uint8_t FRI {1U << 5};
constexpr uint8_t SAT {1U << 6};
constexpr uint8_t WEEKDAYS {MON | TUE | WED | THU | FRI};
constexpr uint8_t WEEKENDS {SAT | SUN};
constexpr uint8_t DAILY    {WEEKDAYS | WEEKENDS};
constexpr uint8_t ONCE     {0};

struct AlarmDef {
  uint8_t hour;    // 0-23, local time
  uint8_t minute;  // 0-59
  uint8_t days;    // day mask, see above
};

// Built-in alarms. These always exist; the web console can turn each one off
// and on (saved to flash) but not delete or move it. Alarms added from the
// web console are kept separately, in flash.
constexpr AlarmDef BUILTIN_ALARMS[] = {
  {7, 0, WEEKDAYS},
  {9, 0, WEEKENDS},
};
constexpr size_t NUM_BUILTIN_ALARMS {sizeof(BUILTIN_ALARMS) / sizeof(BUILTIN_ALARMS[0])};

// Most alarms that can be added from the web console
constexpr size_t MAX_USER_ALARMS {10};

// If the loop falls behind (e.g. a slow NTP sync) or the clock steps forward,
// alarms from up to this many missed minutes still fire.
constexpr int32_t ALARM_CATCHUP {5 * MINUTE};

// An alarm never fires twice within this long. Covers the repeated hour when
// DST ends in the fall.
constexpr int32_t ALARM_REFIRE_GUARD {2 * HOUR};

// Adjustable settings -- defaults /////////////////////////////////////////////
//
// Each of these can be changed from the web console with `set <name> <value>`
// (names in brackets). Changes are saved to flash and override these defaults
// until `defaults` is run.

// Volume profile. Each alarm starts at START volume and ramps linearly to MAX
// over RAMP seconds, then stays there. Volume is 0-100 on a perceptual scale:
// output amplitude follows the square of it, so 50 is about a quarter of full
// amplitude (-12 dB).
constexpr int32_t DEFAULT_START_VOLUME {0};    // % [start_volume]
constexpr int32_t DEFAULT_MAX_VOLUME   {100};  // % [max_volume]
constexpr int32_t DEFAULT_RAMP_S       {60};   // s [ramp]

// Sound: bursts of BEEPS beeps, each BEEP ms long with GAP ms between them,
// then PAUSE ms of silence before the next burst.
enum Waveform : uint8_t { WAVE_SINE, WAVE_SQUARE };
constexpr int32_t DEFAULT_WAVEFORM {WAVE_SINE};  // [wave] sine or square
constexpr int32_t DEFAULT_FREQ_HZ  {1000};       // [freq]
constexpr int32_t DEFAULT_BEEP_MS  {100};        // [beep]
constexpr int32_t DEFAULT_GAP_MS   {100};        // [gap]
constexpr int32_t DEFAULT_BEEPS    {4};          // [beeps]
constexpr int32_t DEFAULT_PAUSE_MS {500};        // [pause]

// Bed behavior. The alarm goes quiet once fewer people are in bed than when
// it started ringing (one person getting up is enough). If the bed gets back
// to that many within AWAY seconds, it rings again; otherwise it's done.
//   UP_CONFIRM:   how long fewer people must read before it goes quiet
//   BACK_CONFIRM: how long the full count must read before it rings again.
//                 Longer than UP_CONFIRM, so someone rolling from one side to
//                 the other (both sides briefly pressed) doesn't count.
//   MAX_RING:     stop ringing after this many minutes of ringing in total,
//                 e.g. if the sensor is down. 0 = never give up.
//   SKIP_EMPTY:   if the bed reads empty when an alarm goes off, don't ring.
//                 If the sensor can't be reached, the alarm rings anyway.
constexpr int32_t DEFAULT_AWAY_S         {10 * MINUTE};  // s [away]
constexpr int32_t DEFAULT_UP_CONFIRM_S   {3};            // s [up_confirm]
constexpr int32_t DEFAULT_BACK_CONFIRM_S {15};           // s [back_confirm]
constexpr int32_t DEFAULT_MAX_RING_MIN   {30};           // min [max_ring]
constexpr int32_t DEFAULT_SKIP_EMPTY     {1};            // on/off [skip_empty]

// Bed occupancy sensor ////////////////////////////////////////////////////////
//
// github.com/TravisLlado/bed-occupancy. Polled only while an alarm is
// ringing or waiting to see if someone gets back in bed. Each side is
// reported separately; this only uses how many sides are occupied.
constexpr const char* BED_OCCUPANCY_URL {"http://bedoccupancy.local/state"};
constexpr uint32_t BED_POLL_INTERVAL_MS {1000};
constexpr uint32_t BED_HTTP_TIMEOUT_MS  {1500};
// Reject a reading whose measurement time is further than this from our own
// clock -- the sensor publishes every second, so an old one means it stalled.
constexpr int32_t  BED_MAX_READING_AGE  {30 * SECOND};

// Audio hardware //////////////////////////////////////////////////////////////
//
// I2S amplifier, e.g. a MAX98357A breakout: BCLK, LRC and DIN to the pins
// below, VIN to 5V. AMP_SD_PIN drives the amp's SD (shutdown) pin so it's off,
// and silent, between alarms; driving it high selects the left channel, and
// both channels carry the same signal. Set it to -1 if SD isn't wired.
constexpr int I2S_BCLK_PIN  {5};
constexpr int I2S_LRCLK_PIN {6};
constexpr int I2S_DOUT_PIN  {7};
constexpr int AMP_SD_PIN    {10};

constexpr uint32_t AUDIO_SAMPLE_RATE {16000};  // Hz

// Full-scale fraction at volume 100. Leaves a little headroom so the amp
// doesn't clip.
constexpr float AUDIO_HEADROOM {0.9F};

// Fade at the start and end of each beep, so beeps don't click
constexpr uint32_t BEEP_EDGE_MS {4};

// Audio task: buffer sizes and priority (above the Arduino loop task's 1, so
// sound never stutters while the loop is waiting on the network)
constexpr size_t   AUDIO_CHUNK_FRAMES  {128};
constexpr int      AUDIO_DMA_BUF_COUNT {4};
constexpr int      AUDIO_DMA_BUF_LEN   {256};
constexpr uint32_t AUDIO_TASK_STACK    {3072};
constexpr UBaseType_t AUDIO_TASK_PRIORITY {5};

// LED /////////////////////////////////////////////////////////////////////////

// Onboard LED (GPIO 8), active low. On while ringing, blinks while waiting to
// see if someone gets back in bed.
constexpr uint8_t LED_PIN {8};

enum LedState : uint8_t {
  LED_ON  = LOW,
  LED_OFF = HIGH,
};

constexpr uint32_t LED_BLINK_MS {500};

// Network /////////////////////////////////////////////////////////////////////

// Web console: http://<MDNS_HOSTNAME>.local/
constexpr const char* MDNS_HOSTNAME {"alarmclock"};
constexpr uint16_t    SERVER_PORT   {80};

// Retry delay (ms) for WiFi and time-sync failures at boot
constexpr uint32_t RETRY_DELAY_MS {5000};

// How often (ms) to check WiFi connectivity and re-join if disconnected
constexpr uint32_t WIFI_CHECK_INTERVAL_MS {60000};

// Geolocation lookup at boot (ip-api.com), used to pick the timezone. If it
// fails this many times, TIMEZONE_POSIX below is used instead.
constexpr uint8_t GEO_ATTEMPTS {3};
constexpr int     GEO_COORD_DECIMALS {4};

// Clock ///////////////////////////////////////////////////////////////////////

// Fallback timezone as a POSIX TZ string, used if the geolocation lookup
// fails or returns a timezone not in the table in WebInfo.cpp. The ESP32
// applies the DST rules in it on its own, so local time stays correct across
// DST changes even if NTP is unreachable. US Pacific: standard time UTC-8, DST
// UTC-7 from the 2nd Sunday of March to the 1st Sunday of November.
constexpr const char* TIMEZONE_POSIX {"PST8PDT,M3.2.0,M11.1.0"};

// NTP servers used to set the clock and correct drift
constexpr const char* NTP_SERVER_1 {"pool.ntp.org"};
constexpr const char* NTP_SERVER_2 {"time.nist.gov"};

// How long (ms) to wait for an NTP response before giving up on a sync
constexpr uint32_t NTP_SYNC_TIMEOUT_MS {10000};

// Local hour (24-hour clock) of the daily NTP re-sync. Runs once per day at or
// after this hour, but never while an alarm is going.
constexpr int DAILY_REFRESH_HOUR {2};

// Console /////////////////////////////////////////////////////////////////////

constexpr uint32_t SERIAL_BAUD_RATE {115200};

// Longest command accepted from USB serial or the web console
constexpr size_t CMD_MAX_LEN {64};

// Status line interval while idle, and while an alarm is going
constexpr uint32_t STATUS_IDLE_INTERVAL_MS   {60000};
constexpr uint32_t STATUS_ACTIVE_INTERVAL_MS {5000};

// `test` command defaults: plays the alarm sound at a fixed volume
constexpr int32_t TEST_DEFAULT_SECONDS {3};
constexpr int32_t TEST_MAX_SECONDS     {60};

// Flash storage ///////////////////////////////////////////////////////////////

// Preferences (NVS) namespaces
constexpr const char* SETTINGS_NAMESPACE {"alarmset"};
constexpr const char* ALARMS_NAMESPACE   {"alarms"};

// Program /////////////////////////////////////////////////////////////////////

constexpr uint32_t TICK_PERIOD_MS {10};

// End of file /////////////////////////////////////////////////////////////////
