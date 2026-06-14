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

`voltage_monitor/voltage_monitor.ino` (fw 2.3):

1. **Battery voltage** — reads a 1 MΩ / 220 kΩ divider on **GPIO1**
   (ADC1_CH0), scaled ×5.545, 64× averaged. 24 h history (1440 samples
   @ 60 s, each timestamped) in PSRAM, snapshotted to LittleFS so it
   survives reboots.
2. **Web dashboard** served by the S3 itself — voltage + chip-temp
   gauges, seven 24 h charts with **per-point hover tooltips** (value +
   when), stat tiles, OTA at `/update`. WiFi STA with an AP fallback.
   **NTP time sync** (`time.windows.com`, re-synced every 6 h) drives a
   live footer clock and the timestamps on the history/tooltips.
3. **CC1101 433 MHz Compustar transmitter** — replays the captured
   1WG3R fixed-code packets to trigger the factory remote start.
   `POST /transmit?button=START|LOCK|UNLOCK|TRUNK`, plus four buttons on
   the dashboard.
4. **Power & performance controls** (NEW in 2.2) — a dashboard card to
   toggle the **CPU clock** (80 / 240 MHz, `POST /cpu?mhz=`) and **WiFi
   power-save** (`POST /wifips?on=`), each showing the current state
   plainly. Both **persist across reboot/brownout** in NVS
   (`Preferences`) and are re-applied at boot — 80 MHz is the WiFi-safe
   floor, so the radio survives the clock drop.

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

## ⚠ RF safety

`/transmit?button=START` replays the engine-start packet. With the CC1101
wired and real patterns loaded, **it cranks the car.** The firmware blocks
transmit unless (a) the CC1101 answers on SPI *and* (b)
`COMPUSTAR_PATTERNS_CAPTURED == 1`. The dashboard START button also
confirms first. Keep patterns placeholder / RF unwired until you're doing
a deliberate, supervised first-trigger test (hood open, per
[`../docs/15-first-trigger.md`](../docs/15-first-trigger.md)).
