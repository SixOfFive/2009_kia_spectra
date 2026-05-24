# 09 — Bench smoke tests for each module

## Goal

Confirm each hardware module works in isolation before you wire them
together. Catches dead modules, swapped pins, and config mismatches
early — much easier to debug one thing at a time than five at once.

## Prerequisites

- [08 — MicroPython flashed](08-flash-micropython.md)
- Breadboard, jumper wires, multimeter
- Modules from the BOM: ESP32, ADS1115, CC1101, SN65HVD230

## Order of operations

Five tests, each ~10 minutes:

1. ESP32 blink-LED (confirms toolchain)
2. ESP32 WiFi scan (confirms RF works at all)
3. ADS1115 reads a known voltage (confirms I2C)
4. CC1101 part-ID register (confirms SPI)
5. SN65HVD230 + `machine.CAN` loopback (confirms TWAI; skip if no CAN
   in your firmware build)

## Step 1 — Copy the project code onto the ESP32

Two options:

**Option A — mpremote** (faster, scriptable):

```powershell
pip install mpremote
esp32\scripts\install.ps1 COM7        # adjust COM port
```

(Or `bash esp32/scripts/install.sh /dev/ttyUSB0` on Linux/macOS.)

**Option B — Thonny GUI**: **View → Files**. The right pane is the ESP32
filesystem. Drag the contents of `esp32/src/` to the ESP32 root:

```
esp32/src/config.py     -> /config.py
esp32/src/controller.py -> /controller.py
esp32/src/main.py       -> /main.py
esp32/src/lib/          -> /lib/
```

Either way, don't copy `secrets.py` yet — create it locally after the SDR
walkthrough recovers the device key.

## Step 2 — Blink LED

Most ESP32 dev boards have an onboard LED on GPIO 2. In the Thonny REPL:

```python
>>> from machine import Pin
>>> import time
>>> led = Pin(2, Pin.OUT)
>>> for _ in range(5):
...     led.on(); time.sleep(0.2); led.off(); time.sleep(0.2)
```

Onboard LED blinks 5 times = toolchain works. If not, suspect a bad
board.

## Step 3 — WiFi scan

```python
>>> import network
>>> wlan = network.WLAN(network.STA_IF)
>>> wlan.active(True)
>>> wlan.scan()
```

Should print a list of nearby SSIDs as tuples. Empty list = the WROOM-32U's
WiFi radio isn't working (rare but happens on counterfeit boards).

## Step 4 — ADS1115 over I2C

Wire:
- ADS1115 VDD → ESP32 3V3
- ADS1115 GND → ESP32 GND
- ADS1115 SCL → ESP32 GPIO 22
- ADS1115 SDA → ESP32 GPIO 21
- ADS1115 ADDR → ESP32 GND (sets I2C address to 0x48)
- ADS1115 AIN0 → ESP32 3V3 (for this test only — produces a known
  full-scale reading)

In REPL:

```python
>>> from lib.ads1115 import ADS1115
>>> adc = ADS1115(i2c_id=0, sda=21, scl=22)
>>> raw = adc.read_single_ended(channel=0)
>>> volts = ADS1115.raw_to_volts(raw)
>>> print("raw=%d  volts=%0.3f" % (raw, volts))
```

Expected: raw ≈ 26400, volts ≈ 3.30. If you see raw = 0 / -1 / wildly
random, recheck wiring (especially SDA/SCL — they get swapped a lot)
and that ADDR is tied to GND.

Bonus check: scan I2C bus to confirm 0x48 is present:

```python
>>> from machine import I2C, Pin
>>> I2C(0, sda=Pin(21), scl=Pin(22)).scan()
```

Should return `[72]` (= 0x48).

## Step 5 — CC1101 over SPI

Wire (default pin assignments — match `main.py`):
- CC1101 VCC  → ESP32 3V3 (**not 5V** — the CC1101 is 3V3 only)
- CC1101 GND  → ESP32 GND
- CC1101 SCK  → ESP32 GPIO 18 (default VSPI SCK)
- CC1101 MISO → ESP32 GPIO 19 (default VSPI MISO)
- CC1101 MOSI → ESP32 GPIO 23 (default VSPI MOSI)
- CC1101 CSn  → ESP32 GPIO 5
- CC1101 GDO0 → ESP32 GPIO 22 (NOTE: also used for I2C — temporarily
  disconnect ADS1115 SCL for this test, or wire CC1101 GDO0 to a
  different free GPIO and update `main.py`)

In REPL:

```python
>>> from lib.cc1101 import CC1101
>>> radio = CC1101(spi_id=2, cs_pin=5, gdo0_pin=22)
>>> radio.reset()
>>> partnum, version = radio.part_info()
>>> print("partnum=0x%02X  version=0x%02X" % (partnum, version))
```

Expected: `partnum=0x00  version=0x14` (or 0x04 on some Chinese clones —
both work). If you get 0xFF for both, SPI isn't communicating — check
MOSI/MISO swap, CS line, and that VCC is exactly 3.3V.

Test transmit (visible on an SDR or another CC1101):

```python
>>> radio.init_433mhz_ook()
>>> from lib import compustar
>>> # Send a dummy packet (won't decode as a real Compustar packet but
>>> # the SDR will see the OOK burst at 433.92 MHz)
>>> packet = compustar.build_packet(
...     serial=0x12345,
...     function_code=0x4,
...     counter=1,
...     device_key=0xDEADBEEFCAFEBABE,
... )
>>> pulses = compustar.packet_to_pulses(packet["bits"])
>>> radio.transmit_pulses(pulses)
```

With your RTL-SDR tuned to 433.92 MHz (from the SDR walkthrough), you
should see a brief OOK burst in the waterfall.

## Step 6 — SN65HVD230 CAN loopback (only if `machine.CAN` is available)

Wire two transceivers to two ESP32s (or one ESP32 to a known-good CAN
device like a Veepeak adapter on a bench supply):

- SN65HVD230 VCC → 3V3
- SN65HVD230 GND → GND
- SN65HVD230 D (driver-in) → ESP32 GPIO 5 (CAN_TX_PIN)
- SN65HVD230 R (receiver-out) → ESP32 GPIO 4 (CAN_RX_PIN)
- CANH to CANH between the two transceivers
- CANL to CANL between the two transceivers
- 120Ω terminator across CANH-CANL at each end

In REPL on one node:

```python
>>> from lib.twai_can import Can
>>> can = Can(tx_pin=5, rx_pin=4, baudrate=500_000)
>>> can.send(0x100, b"Hello!  ")
```

On the other node:

```python
>>> from lib.twai_can import Can
>>> can = Can(tx_pin=5, rx_pin=4, baudrate=500_000)
>>> can.receive(timeout_ms=2000)
(256, b'Hello!  ')
```

Expected: `(0x100, b'Hello!  ')`. If timeout — check CANH/CANL not
swapped, terminator present, both nodes at the same baudrate.

If CAN isn't available in your firmware, skip — `main.py` already
handles `ImportError` and disables OBD polling.

## What you should have when done

- All five (or four) modules confirmed working in isolation
- A mental map of which GPIOs are committed to which peripheral
- Notes on any deviation from the default pinout in `config.py` so you
  can update it accordingly

## Troubleshooting

| Symptom | Fix |
|---|---|
| `OSError: [Errno 19] ENODEV` on I2C | SDA/SCL swapped or pull-ups missing. Add 4.7kΩ to 3V3 on each line. |
| CC1101 `partnum=0xFF version=0xFF` | SPI not connecting. Verify VCC is 3.3V (not 5V — kills the chip). Check MOSI/MISO swap. |
| WiFi scan returns `[]` | Antenna not screwed in (-32U variants need an external IPEX antenna) |
| TX from CC1101 isn't visible on SDR | Recheck `radio.init_433mhz_ook()` was called. PATABLE may be at default 0x00 (no power) — confirm `tx_power` param. |

## Next

[`10-pi-setup.md`](10-pi-setup.md) — flash the Pi, run provision.sh, get
the dashboard up on the touchscreen.
