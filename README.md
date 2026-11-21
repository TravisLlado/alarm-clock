# Alarm Clock

An alarm clock that turns itself off when you get out of bed. An ESP32-C3 SuperMini plays beeps through a MAX98357A I2S amplifier and one speaker. It reads bed occupancy from the [bed occupancy sensor](https://github.com/TravisLlado/bed-occupancy) at `http://bedoccupancy.local/state`.

## Wiring

| MAX98357A | ESP32-C3 SuperMini | Notes |
|---|---|---|
| VIN | 5V | |
| GND | GND | |
| BCLK | GPIO5 | `I2S_BCLK_PIN` |
| LRC | GPIO6 | `I2S_LRCLK_PIN` |
| DIN | GPIO7 | `I2S_DOUT_PIN` |
| SD | GPIO10 | `AMP_SD_PIN`. Low shuts the amp down between alarms, so it doesn't hiss. High selects the left channel, and both channels carry the same signal. If you leave SD unconnected, set `AMP_SD_PIN` to `-1`. The board's onboard pull-up then keeps the amp on all the time |
| GAIN | not connected | Floating gives 9 dB. Tie it to GND for 12 dB or to VIN for 6 dB, if the volume range needs moving |
| Speaker +/− | speaker | 4–8 Ω |

The onboard LED (GPIO8) stays on while the alarm rings and blinks while it waits to see whether you get back in bed. GPIO2, 8 and 9 are strapping pins and aren't used for anything else.

## How an alarm goes

1. At the alarm time it reads the bed. If the bed reads empty, it doesn't ring (`skip_empty`). If the sensor can't be reached, it rings anyway.
2. It plays bursts of beeps. The volume ramps from `start_volume` to `max_volume` over `ramp` seconds (by default 0 → 100 over 60 s), then holds.
3. **It counts people (occupied sides), not which side they're on.** When the count drops below the count at the start for `up_confirm` seconds (1 s, so two readings in a row), it goes quiet. One person getting up is enough.
4. For the next `away` seconds (10 min), if the count gets back to where it started for `back_confirm` seconds (15 s), it rings again. The ramp picks up where it left off. After 10 minutes without that, it's done.

Two people in bed, one gets up: the count goes 2 → 1 and the alarm goes quiet. If the other person then rolls onto the empty side, the count stays 1, so the alarm stays quiet. During a roll both sides can read occupied for a second or two. That's why `back_confirm` is longer than `up_confirm`.

If a second person gets into bed while it's ringing, and stays for `back_confirm`, the count it waits for goes up to 2. After `max_ring` minutes of ringing in total (30), it gives up. That covers a dead sensor or an empty house.

Volume is 0–100 on a perceptual scale. Amplitude follows the square of the volume, so 50 is about a quarter of full amplitude (−12 dB), and the ramp sounds more even than a linear one.

## Console (web + USB serial)

Open **http://alarmclock.local/** (or the IP printed at boot) for the [WebConsole](https://github.com/TravisLlado/WebConsole) terminal. The same commands work over USB serial (`pio device monitor`).

| Command | What it does |
|---|---|
| `status` | Time, timezone, alarm state, next alarm |
| `alarms` | List alarms |
| `alarm add <HH:MM> [days]` | Add an alarm. Days: `daily` (default), `weekdays`, `weekends`, `once`, or a list like `mon,wed,fri` or `mon-thu,sat` |
| `alarm del <n>` | Remove an added alarm |
| `alarm on <n>` / `alarm off <n>` | Turn an alarm on or off |
| `settings` | List the adjustable settings. `*` marks ones changed from the default |
| `set <name> <value>` | Change a setting, e.g. `set ramp 90`, `set wave square` |
| `defaults` | Put every setting back to its `config.h` default |
| `ring` | Start an alarm now, with the full ramp and bed logic |
| `test [volume] [seconds]` | Play the sound at a fixed volume (default `max_volume`, 3 s) |
| `stop` | Stop the alarm or the test |

A `once` alarm fires at the next occurrence of its time and then turns itself off. It stays in the list so you can turn it back on.

A status line prints every minute while idle and every 5 s while an alarm is going, for example `bed 1/2` means one person in bed out of the two it's waiting for.

### Settings

| Name | Default | Meaning |
|---|---|---|
| `start_volume` | 0 % | Volume when an alarm starts |
| `max_volume` | 100 % | Volume at the end of the ramp |
| `ramp` | 60 s | Time from start to max volume |
| `wave` | sine | `sine` or `square` |
| `freq` | 1000 Hz | Beep pitch |
| `beep` | 100 ms | Length of each beep |
| `gap` | 100 ms | Silence between beeps in a burst |
| `beeps` | 4 | Beeps per burst |
| `pause` | 500 ms | Silence after each burst |
| `away` | 600 s | After getting up, it rings again if you're back in bed within this |
| `up_confirm` | 1 s | Fewer in bed for this long counts as getting up |
| `back_confirm` | 15 s | Back in bed for this long and it rings again |
| `max_ring` | 30 min | Give up after this much ringing in total (0 = never) |
| `skip_empty` | on | Don't ring if the bed is empty at alarm time |

## Alarms in config.h

Built-in alarms are in `BUILTIN_ALARMS` in `src/config.h`:

```cpp
constexpr AlarmDef BUILTIN_ALARMS[] = {
  {6, 0, WEEKDAYS},
  {7, 0, WEEKENDS},
};
```

Days combine with `|`, e.g. `MON | WED | FRI`. Built-ins can be turned off and on from the console but not removed. Alarms added from the console are stored separately, up to `MAX_USER_ALARMS` (10). Everything is saved to flash (NVS) and survives reboots and reflashing. If you edit `BUILTIN_ALARMS`, every built-in starts out on again after flashing.

The defaults for every setting are in `src/config.h` too. A value set from the console overrides its default until you run `defaults`.

## Clock

There's no battery-backed clock. At boot it joins Wi-Fi and looks up its location from its public IP (ip-api.com, same as the blinds controller). It uses the location's timezone to pick POSIX TZ rules from the table in `src/WebInfo.cpp`, then sets the clock from NTP (`pool.ntp.org`, `time.nist.gov`). If the lookup fails, or the timezone isn't in the table, it uses `TIMEZONE_POSIX` from `config.h` (US Pacific). DST is handled onboard from the TZ rules.

Boot waits until the first NTP sync succeeds. After that, alarms keep working without Wi-Fi; only the bed sensor and the console need it. It re-syncs every night at 02:00, but never while an alarm is going. On the night DST ends, an alarm in the repeated hour fires once.

## Build & flash

Wi-Fi credentials come from the shared `credentials.h`, found through the `CREDENTIALS_INCLUDE` environment variable (same setup as the blinds and bed occupancy projects).

```sh
pio run -t upload
pio device monitor
```
