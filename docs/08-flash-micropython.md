# 08 — Flash MicroPython onto the ESP32

## Goal

Get a MicroPython REPL responding over USB on the ESP32-WROOM-32U so you
can copy the firmware files in step 09.

## Prerequisites

- ESP32-WROOM-32U board (one of the legitimate variants — see the BOM
  scam warning, **not** the $3 antenna-only variant)
- USB-A to micro-USB cable (data, not charge-only)
- Thonny installed (`pip install thonny` or download from
  [thonny.org](https://thonny.org))

## Step 1 — Plug in and confirm the COM port

Plug the ESP32 in. On Windows, check Device Manager → Ports (COM & LPT).
You should see a new entry like `Silicon Labs CP210x USB to UART Bridge
(COM7)` or `USB-SERIAL CH340 (COM7)`.

If no port appears, the cheap clone boards often need the CP210x or CH340
USB-serial driver:
- CP210x: [silabs.com](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers)
- CH340: [wch-ic.com](https://www.wch-ic.com/downloads/CH341SER_EXE.html)

## Step 2 — Download MicroPython firmware

Get the latest stable build for ESP32 (generic):

  https://micropython.org/download/esp32/

Pick the `.bin` file ending in `-v1.24.x.bin` (or newer). Save to a known
path, e.g. `Downloads\esp32-v1.24.bin`.

## Step 3 — Flash with Thonny

1. Open Thonny.
2. **Tools → Options → Interpreter**:
   - Which interpreter: **MicroPython (ESP32)**
   - Port: select the COM port from step 1
   - Click **Install or update MicroPython (esptool)**
3. In the dialog:
   - Target port: same COM
   - MicroPython family: **ESP32 / WROOM**
   - Variant: **ESP32-WROOM-32U** (or generic ESP32 if WROOM-32U
     isn't listed — the firmware is the same)
   - Click **Install**
4. Wait ~30 seconds. The board's blue LED will flicker during flash.

When the dialog closes, you should see `MicroPython v1.24...` printed in
the Thonny shell pane.

## Step 4 — Verify the REPL

In the Thonny shell pane, type:

```python
>>> import sys
>>> print(sys.implementation)
```

Expected output:

```
(name='micropython', version=(1, 24, 0, '', ''), _machine='ESP32 module
with ESP32', _mpy=...)
```

If you see this, MicroPython is running and the USB serial bridge works.

## Step 5 — Verify CAN/TWAI support (if your firmware build includes it)

Our `lib/twai_can.py` driver expects `machine.CAN`. Test:

```python
>>> from machine import CAN
```

- **No ImportError**: your firmware has CAN support. Good — proceed.
- **ImportError**: standard MicroPython doesn't include CAN. Options:
  - Use a community fork that builds with CAN enabled (Lemariva /
    loboris / etc.)
  - Build MicroPython from source with `make BOARD=ESP32_GENERIC
    USER_C_MODULES=...twai`
  - Accept that OBD polling will be disabled — the controller degrades
    gracefully (CAN becomes a no-op in main.py).

For a v1 build the third option is acceptable. OBD polling is a
nice-to-have, not a requirement for the voltage-trigger feature.

## What you should have when done

- ESP32 plugged in and recognized as a COM port
- MicroPython REPL responding in Thonny
- `sys.implementation` confirms MicroPython on ESP32
- Awareness of whether `machine.CAN` is available in your build

## Next

[`09-bench-smoke-tests.md`](09-bench-smoke-tests.md) — wire each module to
the ESP32 on a breadboard and confirm each one works in isolation before
combining them.
