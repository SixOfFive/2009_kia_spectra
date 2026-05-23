# ESP32 firmware

MicroPython firmware for the always-on monitor + RF transmit + CAN bus interface.

## Status

Not started. Awaiting parts.

## Planned modules

- `boot.py` — runs on every boot, sets up WiFi credentials, brings up hardware
- `main.py` — main loop, deep-sleep cycle
- `lib/`
  - `ads1115.py` — battery voltage ADC driver
  - `cc1101.py` — Sub-GHz transceiver driver (SPI)
  - `keeloq.py` — Keeloq cipher encoder
  - `compustar.py` — High-level Compustar packet construction
  - `twai_can.py` — Native CAN bus interface
  - `obd2.py` — OBD-II PID query helpers
  - `pi_link.py` — UART comms with Pi Zero
  - `mqtt.py` — Optional direct MQTT publish (when Pi is asleep)

## Development environment

- Thonny IDE (recommended for beginners)
- OR: ampy / rshell / mpremote from command line
- USB connection for flashing + REPL
- MicroPython firmware: latest stable for ESP32 (download from micropython.org)

## Flashing MicroPython

```bash
# One-time: install esptool
pip install esptool

# Erase the flash
esptool.py --chip esp32 --port COM3 erase_flash

# Flash MicroPython firmware
esptool.py --chip esp32 --port COM3 --baud 460800 write_flash -z 0x1000 esp32-XXXXX.bin
```

Replace `COM3` with whatever port your ESP32 enumerates as (Windows Device Manager will show it).

## Notes

To be expanded as code is written.
