# esp32-s3 — the actual ESP32-S3 Arduino firmware

This is the **real hardware build** of vroom's ESP32 half: an
**ESP32-S3-WROOM-1-N16R8** running an Arduino/C++ sketch. It supersedes
the MicroPython reference tree in [`../esp32/`](../esp32/) (which targets
the originally-planned ESP32-WROOM-32U + ADS1115). Both are kept: the
MicroPython tree documents the design and has the CPython unit tests; this
tree is what's flashed to the board on the bench.

See [`logs/2026-06-09.md`](../logs/2026-06-09.md) for the bring-up story
and the divergence rationale.

## What it does

`voltage_monitor/voltage_monitor.ino` (fw 2.6):

1. **Battery voltage** — reads a 1 MΩ / 220 kΩ divider on **GPIO1**
   (ADC1_CH0), scaled ×5.545, 64× averaged. 24 h history (1440 samples
   @ 60 s, each timestamped) in PSRAM, snapshotted to LittleFS so it
   survives reboots.
2. **Web dashboard** served by the S3 itself — voltage + chip-temp
   gauges, nine 24 h charts (incl. both CPU cores) with **per-point hover
   tooltips** (value + when), stat tiles, OTA at `/update`. WiFi STA with
   an AP fallback.
   **NTP time sync** (`time.windows.com`, re-synced every 6 h) drives a
   live footer clock and the timestamps on the history/tooltips.
3. **CC1101 433 MHz Compustar transmitter** — replays the captured
   1WG3R fixed-code packets to trigger the factory remote start.
   `POST /transmit?button=START|LOCK|UNLOCK|TRUNK`, plus four buttons on
   the dashboard.
4. **Power & performance controls** — a dashboard card to
   toggle the **CPU clock** (80 / 240 MHz, `POST /cpu?mhz=`) and **WiFi
   power-save** (`POST /wifips?on=`), each showing the current state
   plainly. Both **persist across reboot/brownout** in NVS
   (`Preferences`) and are re-applied at boot — 80 MHz is the WiFi-safe
   floor, so the radio survives the clock drop. Also reports true
   **per-core CPU load** (`cpu0` / `cpu1` in `/json`), read from the
   FreeRTOS idle-task run-time counters.
5. **Low-voltage auto-start** (NEW in 2.4, opt-in — **ships disabled**) —
   fires the remote start by itself once battery voltage stays below a
   threshold for a sustained period, so the alternator can recharge the
   battery before it's too flat to crank. Defaults: **below 12.4 V held
   for 60 s**, 2 h cooldown. Config + the start log persist to NVS /
   LittleFS. `POST /autostart?en=&volts=&hold=&cool=`, `GET /starts`.
   See the safety section below.

## Layout

```
esp32-s3/
  voltage_monitor/
    voltage_monitor.ino     the firmware
    cc1101_compustar.h/.cpp  self-contained CC1101 driver + 1WG3R renderer
    partitions.csv          16 MB OTA-capable layout (2x3MB app + ~9.9MB FS)
    secrets.h.example        template — copy to secrets.h (gitignored)
  docs/voltage-monitor.md   full project doc + wiring diagram
  python/voltage_client.py  stdlib CLI poller for /json
```

## Wiring

**Voltage divider** (battery sense): `V+ → 1 MΩ → [node] → 220 kΩ → GND`,
node → GPIO1, with a 100 nF cap node→GND. On the bench, `V+` is the 3.3 V
rail; in the car it's a fused battery+ tap. See
[`docs/voltage-monitor.md`](docs/voltage-monitor.md).

**CC1101** (3.3 V logic only — *not* 5 V tolerant):

| CC1101 | ESP32-S3 GPIO |
|---|---|
| SCK  | 18 |
| MISO | 17 |
| MOSI | 16 |
| CSn  | 15 |
| GDO0 | 4  |
| VCC  | 3V3 |
| GND  | GND |

433 MHz quarter-wave whip (~17 cm) or SMA antenna on the CC1101's antenna
pad. Pins avoid the ADC pin (GPIO1), strapping pins, flash/PSRAM pins,
USB, UART0, the RGB LED, and the (disabled) display pins — change them at
the top of the `.ino` if your board differs.

## Build + flash

The Arduino toolchain (`arduino-cli` + the esp32 core + libraries) is
**not** committed — it's multi-GB and reproducible. Install it once
(arduino-cli, then `arduino-cli core install esp32:esp32`, plus the
Adafruit GFX/ILI9341/BusIO + XPT2046_Touchscreen libs the display code
references even while disabled).

```sh
# compile
arduino-cli compile \
  --fqbn esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M \
  esp32-s3/voltage_monitor

# first/structural flash over USB (CH343 UART port):
arduino-cli compile --fqbn esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M \
  -u -p COM6 esp32-s3/voltage_monitor

# subsequent app updates: OTA, no cable —
#   browse http://esp32-volt.local/update and upload the .bin
```

## secrets.h

Copy `voltage_monitor/secrets.h.example` to
`voltage_monitor/secrets.h` (gitignored) and fill in:

- **WiFi** SSID + password, and the AP fallback password.
- **Compustar patterns** — the four 35-bit strings your FOB sends, from an
  SDR capture (`sdr/scripts/capture-to-secrets.py`). Set
  `COMPUSTAR_PATTERNS_CAPTURED 1` only after pasting real values.

Without `secrets.h` the firmware still builds and runs (placeholder WiFi,
RF disabled). With placeholder Compustar patterns,
`COMPUSTAR_PATTERNS_CAPTURED 0` keeps `/transmit` blocked so a bench board
can never emit a bogus packet.

## Low-voltage auto-start

Off unless you arm it. When armed, the firmware can crank the engine with
nobody present, so it is deliberately hard to trigger by accident — **every**
one of these must hold before a packet goes out:

| Guard | Why |
|---|---|
| `as_en` is set (dashboard toggle) | Opt-in; the default is disabled |
| RF armed — CC1101 present *and* real patterns loaded | Can't half-fire |
| ≥ 2 min since boot | ADC settle; a brownout-reboot can't fire instantly |
| Reading inside **8–16 V** | A bench rig or unplugged sense wire reads outside this and is treated as *no information*, never as "low" |
| Voltage **strictly below** the threshold, continuously for the hold time | Rejects the 1–3 s dip while the engine is actually cranking |
| ≥ 15 min continuously below 13.2 V | Proves the alternator is off, i.e. the car is parked — it won't fire while you're driving |
| Cooldown elapsed (default 2 h) | Anti-loop; persists across reboot via wall-clock |
| Battery recovered to (threshold + 0.15 V) for 10 min since the last start | Hysteresis — without it a battery sitting just above the trigger re-fires forever. Relative to the threshold, and it times out after 2 cooldowns, so it can delay a start but never block one permanently |
| Under the 24 h cap, **if** one is set | **Off by default** (`0` or `−1` = unlimited) — see below |
| Not locked out | Two consecutive starts that draw no charge latch it off |

After any start (manual or automatic) the firmware watches for the alternator
to come up within 3 minutes. That's the only real proof the engine caught, and
it's recorded per-event in the start log as *ran* / *no charge*. Two automatic
starts in a row with no charge means the car isn't going to start — it latches
`lockout` and stops cranking until you clear it. A **manual** press never counts
toward that lockout, so bench testing can't disable the automatic system.

**Default 12.4 V, not 12.2 V.** In a cold climate the engine wants roughly
double the cranking torque near −20 °C while the battery delivers about half its
power, and a battery down at 12.2 V has electrolyte that slushes around −26 °C.
In a mild climate 12.2 V is fine — it's one field on the page.

**No 24 h cap by default.** This car has a known parasitic drain that has
already destroyed two batteries by deep-discharge sulfation. Capping the number
of starts would mean *choosing* to let the battery sit flat once the cap is hit
— which is the exact failure the project exists to prevent, and the thing that
kills lead-acid. The runaway protection that matters is the **lockout**: if two
starts in a row draw no charge, the engine isn't catching and further cranking
achieves nothing, so it latches off. That distinguishes "the battery legitimately
needs frequent help" from "something is broken"; a fixed count can't. Set a cap
only if you specifically want a ceiling.

> **⚠ Never leave auto-start armed with the car parked in an attached garage or
> any enclosed space.** It will start the engine unattended, and exhaust in an
> enclosed space is lethal.

## ⚠ RF safety

`/transmit?button=START` replays the engine-start packet. With the CC1101
wired and real patterns loaded, **it cranks the car.** The firmware blocks
transmit unless (a) the CC1101 answers on SPI *and* (b)
`COMPUSTAR_PATTERNS_CAPTURED == 1`. The dashboard START button also
confirms first. Keep patterns placeholder / RF unwired until you're doing
a deliberate, supervised first-trigger test (hood open, per
[`../docs/15-first-trigger.md`](../docs/15-first-trigger.md)).
