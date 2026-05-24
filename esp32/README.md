# ESP32 firmware

MicroPython firmware for the always-on monitor + RF transmit + CAN bus interface.

## Status

Implementation complete. 81 hardware-independent tests passing. Awaiting parts for bench validation.

## Modules

- `main.py` — boot entrypoint
- `controller.py` — state-machine glue (MONITORING / STARTING / RUNNING / STOPPING / COOLDOWN)
- `config.py` — non-secret tunables
- `secrets.py` — WiFi creds, MQTT creds, captured FOB packets (gitignored; copy from `secrets.py.example`)
- `lib/`
  - `ads1115.py` — battery voltage ADC driver (I2C)
  - `cc1101.py` — sub-GHz transceiver driver (SPI + GDO0 async OOK)
  - `compustar.py` — Compustar 1WG3R-family fixed-code packet builder. Captures and replays per-button 35-bit patterns verbatim
  - `keeloq.py` — KeeLoq cipher (kept for HCS-KeeLoq variants; **unused by the main 1WG3R-family path**)
  - `twai_can.py` — native CAN bus interface
  - `obd2.py` — OBD-II PID query helpers
  - `pi_link.py` — UART comms with Pi Zero (line-delimited JSON)
  - `persistence.py` — RTC-memory helpers for surviving deep sleep
- `scripts/` — install + simulate helpers
- `tests/` — pure-CPython unit tests (mocked hardware)

## Development environment

- Thonny IDE (recommended for beginners)
- OR: ampy / rshell / mpremote from command line
- USB connection for flashing + REPL
- MicroPython firmware: latest stable for ESP32 (download from micropython.org)

See [`docs/08-flash-micropython.md`](../docs/08-flash-micropython.md) for the full flash + provision walkthrough.

## Running the tests

```powershell
# All ESP32 tests
foreach ($f in Get-ChildItem esp32/tests/test_*.py) { python $f.FullName }
```

Expected total: 81 tests passing (23 compustar + 17 controller + 9 integration + 4 keeloq + 6 cc1101 + 8 ads1115 + 5 persistence + 9 obd2).
