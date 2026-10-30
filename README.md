# fanctl

Temperature-based fan control for Apple Silicon Macs: a small C program that reads the
temperature sensors and writes fan targets straight to the SMC. No app, no kernel extension.

[中文说明](README.zh-CN.md)

## Install

Requires an Apple Silicon Mac with macOS 14 or later. In Terminal:

```sh
curl -fsSL https://github.com/itswl/fanctl/releases/latest/download/install.sh | sh
```

or, with wget:

```sh
wget -qO- https://github.com/itswl/fanctl/releases/latest/download/install.sh | sh
```

The installer is a single file that carries the prebuilt binary; nothing is cloned or compiled.
It asks for your password, installs `fanctl`, the presets, the config (the **balanced** preset)
and a LaunchDaemon, starts it and prints the status. Run the same command again to upgrade; your
config and autostart setting are kept. If you'd rather read it first, download `install.sh` and run `sh install.sh`.

To start with another preset (`quiet`, `balanced`, `cool` or `lap`), add `-s -- --preset NAME`;
the old config is kept as `fancontrol.conf.bak`:

```sh
curl -fsSL https://github.com/itswl/fanctl/releases/latest/download/install.sh | sh -s -- --preset lap
```

Uninstall with `sudo /usr/local/share/fanctl/uninstall.sh` (`--purge` also deletes the config and
the log). The fans go back to macOS.

## Presets

| Preset | Takes over at | For |
|---|---|---|
| `balanced` (default) | 55 °C | Everyday use. Light loads stay with macOS, so the fans can stop; under sustained load they start a little earlier and run a little faster. |
| `quiet` | 62 °C | Less noise. Below 62 °C the fans are left to macOS; long full loads run a few degrees warmer. |
| `cool` | 48 °C | Long builds, renders, model runs. Audible under medium load. |
| `lap` | 40 °C | On your lap: the fans never stop, which keeps the bottom of the case cool. Faint noise at idle. |

`sudo fanctl preset quiet` switches presets; it takes effect at once, and a config you edited by
hand is saved as `fancontrol.conf.bak` first. `fanctl preset` lists them and marks the active one.

## Usage

```sh
fanctl status                    # temperatures, fans (manual/auto), preset, daemon, autostart
fanctl preset                    # list the presets (* = active)
sudo fanctl preset quiet         # switch preset, takes effect at once
sudo fanctl stop                 # stop now; the fans go back to macOS
sudo fanctl start                # start again
sudo fanctl autostart off        # don't start at boot (on: start at boot, the default)
sudo fanctl reload               # after editing /usr/local/etc/fancontrol.conf
fanctl temp                      # the PMU temperature the curve uses
fanctl run --dry-run --verbose   # what the daemon would do, without touching the fans
```

`stop` and `start` change only what runs now; `autostart` decides whether fanctl starts at boot.

### Manual control

The running daemon overrides manual settings, so stop it first and start it again when you are
done:

```sh
sudo fanctl stop          # stop the daemon (the fans go back to macOS)
sudo fanctl set 3000      # pin every fan to 3000 rpm
sudo fanctl set 1 2600    # or just fan 1
sudo fanctl auto          # hand the fans back to macOS
sudo fanctl start         # back to the curve
```

## Configuration

`/usr/local/etc/fancontrol.conf`:

```
interval 3            # seconds between checks while fanctl drives the fans
auto_below 55         # below this PMU temperature macOS keeps the fans (they may stop)
auto_hysteresis 5     # hand back only below 55 - 5 = 50 °C, so it does not flap
confirm_up 5          # a rise must show on 5 checks in a row before fanctl reacts (default)
confirm_down 20       # a fall must show on 20 checks in a row (default)
cpu_guard 90 100      # CPU guard, this is the default; "cpu_guard off" disables it

curve_pct 50 1        # <°C> <percent of each fan's range: 0 = minimum, 100 = maximum>
curve_pct 60 13       # linear in between, flat beyond the ends
curve_pct 70 35
```

- **The temperatures are PMU temperatures, not CPU core temperatures.** On a 14-inch M3 Pro the
  PMU reads about 45 °C at idle and 50–60 °C under everyday load, while the CPU cores may already
  be above 85 °C. See [How it works](#how-it-works).
- `curve <°C> <rpm>` takes absolute speeds instead of `curve_pct`; use one or the other. Absolute
  speeds only fit one model, percentages fit any Mac.
- `cpu_guard <start> <full>`: while fanctl drives the fans and the smoothed CPU temperature is
  above `<start>`, the fans run at least at (temperature − start) / (full − start) of their range,
  100 % at `<full>`. Without it, the slow PMU temperature would keep the fans low during long
  full loads.
- Keep `auto_below` above the idle temperature, or the fans never stop (`lap` does this on purpose).
- After editing, run `sudo fanctl reload`. It checks the file first; an invalid file is rejected
  and the old config stays in effect.

## How it works

### Not reacting to every bump

A change has to show on several checks in a row before fanctl acts on it, and then the fans
move gradually:

| | Confirmed by | Then |
|---|---|---|
| Taking over | `confirm_up` checks at or above `auto_below` (default 5, about 12 s) | starts from the speed the fans are already at |
| Faster fans | `confirm_up` checks wanting more | ramps up by 2 % of the range per second (about 50 s from minimum to maximum) |
| Slower fans | `confirm_down` checks wanting less (default 20, about 1 min) | ramps down by 0.5 % per second (about 3 min back) |
| Handing back | `confirm_down` checks below `auto_below - auto_hysteresis` | macOS takes over from the current speed |

While a takeover is being confirmed macOS keeps cooling as usual. The one exception: when the
smoothed CPU temperature reaches the `cpu_guard` full point, the fans go to full speed at once.

### Temperatures

| Source | Sensors | Behaviour (14-inch M3 Pro) | Used for |
|---|---|---|---|
| HID | maximum of `PMU tdie*` (`PMU tdev*` if there are none) | slow and steady: +3 °C after 25 s with five P-cores busy | curve, `auto_below` |
| SMC | `TCMz` (P-core maximum) and `Tg*` (GPU) | fast and spiky: 73 → 85 °C within 2 s of the same load, 65–80 °C jumps at idle | `cpu_guard`, smoothed with a 10 s time constant |

Apple Silicon has no CPU core sensors in HID, and Intel-era SMC keys such as `TC0P` are gone. SMC
key names change between chip generations, so fanctl finds the CPU and GPU keys by prefix at
start-up and uses `TCMz` only after checking that it matches the maximum of the `Tp*` keys;
otherwise it reads every `Tp*`/`Te*`/`Tg*` key.

### Fans

| Key | Meaning |
|---|---|
| `F<i>Ac` | actual speed |
| `F<i>Mn` / `F<i>Mx` | speed range |
| `F<i>Tg` | target speed (`flt`) |
| `F<i>Md` | mode (`ui8`): **1 = manual, 0 = SMC** |

Easy to get wrong:

1. A target is ignored unless `F<i>Md` is 1 first, and the write still reports success. Handing a
   fan back means writing `F<i>Md = 0`; a zero target alone does nothing.
2. `F<i>Md` is one byte. Written as a float, 1.0 puts 0x00 in the first byte.
3. A new target takes 30–100 ms to land; reading it back too early looks like a failure.
4. Without root the SMC also reports success and does nothing, so every write is read back.
5. The SMC throttles rapid writes. fanctl writes only when the target moves by more than 100 rpm,
   writes all fans in one batch and leaves at least 2 s between batches.
6. If the system takes a fan back (`F<i>Md` reads 0) or its target changes underneath, fanctl
   takes it over again on the next check.

### Keeping the footprint small

Every sensor read is a call into the kernel, so fanctl reads only what the current state needs:

- While macOS has the fans, only the PMU temperature is read, and the next check is further away
  the further the temperature is from `auto_below` (it rises at most ~0.15 °C/s; fanctl assumes
  0.5 °C/s), up to 30 s.
- While fanctl drives the fans: `TCMz` and the fan modes every `interval`, the PMU temperature
  every 2nd check (every check while a hand-back is being confirmed), the GPU sensors every 5th,
  the fan targets every 10th. Speed ranges are read once.
- Signals arrive through kqueue: the process wakes once per check, or at once for a signal, and
  never polls.
- The log is written only when something changes: start, reload, a mode switch, a curve change
  of more than 3 %, the guard engaging or releasing, and failures at most once a minute.

Measured over 60 s, compared with reading every sensor every 3 s:

| | context switches | system calls | CPU time |
|---|---|---|---|
| macOS in charge (`balanced` at idle) | 4,439 → 172 | 2,510 → 211 | 63 → 12 ms |
| fanctl in charge (`lap`) | 4,592 → 563 | 2,710 → 590 | 65 → 20 ms |

Resident memory is about 3 MB.

### Other details

- If the temperature cannot be read three times in a row, the fans go back to macOS.
- No signal handlers (IOKit is not async-signal-safe): TERM/INT/HUP are collected through kqueue
  and handled in the main loop. TERM hands the fans back before exiting, HUP reloads the config.
- The LaunchDaemon uses `ProcessType` `Standard` (utility QoS, priority 20). With every core busy,
  a 500 ms sleep wakes 180–210 ms late on average as `Background` (priority 4), 56 ms late as
  `Standard`.
- Only one `fanctl run` can drive the fans at a time (`/var/run/fanctl.lock`).

## Notes

- fanctl writes real hardware state and needs root; uninstalling hands the fans back to macOS.
- Don't run other fan-control software at the same time; they would fight over the fans.
- A hand-written curve is not smarter than the built-in control, which sees more sensors and
  reacts faster. fanctl is for preferences: quieter, cooler, or steady airflow.
- Intel Macs are not supported; on Macs without fans the daemon exits.
- Tested on a 14-inch MacBook Pro (M3 Pro, Mac15,6, 2 fans, 2317–6800 rpm) with macOS 14.4.
